# Life of a transaction

This document describes how Dragonfly transactions provide atomicity and serializability for its multi-key and multi-command operations.

Note that simple, single-key operations are already linearizable in a shared-nothing architecture because each shard-thread performs operations on its keys sequentially.
The complexity rises when we need to provide strict-serializability (aka serializability and linearizability) for multiple keys.

Transactions in Dragonfly are orchestrated by an abstract entity, called coordinator.
In reality, a client connection instance takes on itself the role of a coordinator: it coordinates transactions every time it executes a redis or memcached command. The algorithm behind Dragonfly transactions is based on the [VLL paper](https://www.cs.umd.edu/~abadi/papers/vldbj-vll.pdf).

Every step within a coordinator is done sequentially. Therefore, it's easier to describe the flow using a sequence diagram. Below is a sequence diagram of a generic transaction consisting of multiple execution steps. In this diagram, the operation it executes touches keys in two different shards: `Shard1` and `Shard2`.

```mermaid
%%{init: {'theme':'base'}}%%
sequenceDiagram
    participant C as Coordinator
    participant S1 as Shard1
    participant S2 as Shard2

    par hop1
    C->>+S1: Schedule
    and
    C->>+S2: Schedule
    S1->>C: Ack
    S2->>C: Ack
    end

    par hop2
    C->>S1: Exec1
    and
    C->>S2: Exec1
    S1--)C: Ack
    S2--)C: Ack
    end
    par hop N+1
    C->>S1: Exec N
    and
    C->>S2: Exec N
    S1--)-C: Ack
    S2--)-C: Ack
    end
```

The shared-nothing architecture of Dragonfly does not allow accessing each shard data directly from a coordinator fiber. Instead, the coordinator sends messages to the shards and instructs them what to do at each step. Every time, the coordinator sends a message, it blocks until it gets an answer. We call such interaction a *message hop* or a *hop* in short.

The flow consists of two different phases: *scheduling* a transaction, and *executing* it. The execution phase may consist of one or more hops, depending on the complexity of the operation we model.

*Note, that when a coordinator fiber is blocked, its thread can still execute other tasks - like processing requests in other connections or handle operations for the shard it owns. This is the advantage of adopting fibers - they allows us to separate the execution context from OS threads.*

## Scheduling a transaction

The transaction initiates with a scheduling hop, during which the coordinator sends to each shard the keys that shards touches. The coordinator sends messages to multiple shards concurrently but it waits until all shards ack and confirm that the scheduling succeeded before it proceeds to the next steps.

Once the scheduling message is processed by a receiving shard, it adds the transaction to its local transaction queue (tx-queue). In order to provide serializability, i.e. to make sure that all shards order their scheduled transactions in the same order, Dragonfly maintains a global sequence counter that is used to induce a total order for all its transactions.

This global counter is shared by all coordinator entities and is represented by an atomic integer. *This counter may be a source of contention - it breaks the shared nothing model, after all. However, in practice, we have not observed a significant impact on Dragonfly performance due to other optimizations we added. These will be detailed in sections below*.

The tx-queue in each shard is ordered by the seq-number of each transaction. It may be that scheduling messages will arrive to each shard in a different order, which may cause that transactions with larger id be scheduled before transactions with a smaller id. If the scheduling algorithm fails to reorder the last transaction, it fails the scheduling request and the coordinator retries the whole hop again by allocating a new sequence number. In reality the fail-rate of a scheduling attempt is very low and the retries happen very rarely.

Once the transaction is added to the tx-queue, the shard also marks the keys it touches using the *intent* locks. Those locks do not block the flow of the underlying oepration but mere express the intent to touch or modify the key. In reality, they are represented by a map: `lock:str->counter`. If `lock[key] == 2` it means the tx-queue has 2 pending transactions that plan to modify `key`. These intent locks are used for optimizations detailed below and are not required to implement the naive version of VLL algorithm.

Once the scheduling hops converges, it means that the transaction entered the execution phase, in which it never rollbacks, or retries. Once it's been scheduled, VLL guarantees the progress of subsequent execution operations while providing strict-serializability guarantees.

It's important to note that a scheduled transaction does not hold exclusivity on its keys. There could be other transactions that still mutate keys it touches - these transactions were scheduled earlier and have not finished running yet, or even have not even started running.

## Executing a transaction

Once the transaction is scheduled, the coordinator starts sending the execution messages. We break each command to one or more micro-ops and each operation corresponds to a single message hop.

For example, "MSET" corresponds to a single micro-op "mset" that has the same semantics, but runs in parallel on all the involved shards.

However, "RENAME" requires two micro-ops: fetching the data from two keys, and then the second hop - deleting/writing a key (depending whether the key is a source or a destination).

When a coordinator schedules an execution, it also specifies whether this execution is the last hop for that command. This is necessary, so that shards could do the  clean-ups when running the last execution request.

Once a coordinator sends the micro-op request to all the shards, it waits for the answer. Only when all shards executed the micro-op and return the result, the coordinator is unblocked and it can proceed to the next hop. The coordinator is allowed to process the intermediary responses from the previous hops in order to define the next execution request.

The shards always execute transactions at the head of the tx-queue. When the last execution hop for that transaction is executed the transaction is removed from the queue and the next one can be executed. This way we maintain the ordering guarantees specified by the scheduling order of the transactions and we maintain the atomicity of operations across multiple shards.

## Multi-op transactions (Redis transactions)

Redis transactions (MULTI/EXEC sequences) are modelled as `N` commands within a DF transaction. In order to avoid ambiguity with terms, we call a Redis transaction -  multi-transaction in Dragonfly.

Dragonfly transactional framework allows running any command as standalone transaction or within a multi-transaction. The complexity of handling both cases is pushed into the transactional framework and a command implementation is not aware of a transaction context. Take for example, redis transaction consisting of 2 commands:

```
MULTI
SET x foo
SET y bar
EXEC
```

`SET` command can be implemented with two hops (below is a pseudo-code):

```cpp
trans->Schedule();   // Hop1

trans->Exec(trans, [](key, val) {
  dict[key] = val;
});   // Hop2
```

With multi-transactions we would run the following code that includes both commands.

```cpp
// set x foo
trans->Schedule();

trans->Exec([](key, val) {
  dict[key] = val;    // x <- foo
});

// set y bar
trans->Schedule();
trans->Exec([](key, val) {
  dict[key] = val;    // y <- bar
});
```

However we want to schedule our mult-transaction only once and then set the arguments for each command accordingly. The transactional framework tracks the progress of multi-transaction - it exits early from the second call to "Schedule()" (makes is a noop), and it switches the arguments to each Exec call according to the transaction progress behind the scenes.


## Optimizations

## Blocking commands (BLPOP)

Redis has a rich api with around 200 commands. Few of those commands provide blocking semantics, which allow using Redis as publisher/subscriber broker.

Redis (when running as a single node) is famously single threaded, and all its operations are strictly serializable. In order to build a multi-threaded memory store with equivalent semantics as Redis, we need to design an algorithm that can parallelize potentially blocking operations and still provide strict serializability guarantees. This section focuses mainly on how to solve this challenge for BLPOP (BRPOP) command since it involves coordinating multiple keys and is considered the more complicated case. We assume that solving the problem for `BLPOP` will solve it for any other blocking command.

Background reading:
* [Fauna Serializability vs Linearizability](https://fauna.com/blog/serializability-vs-strict-serializability-the-dirty-secret-of-database-isolation-levels)
* [Jepsen consistency diagrams](https://jepsen.io/consistency)
* [Strict Serializability definition](https://jepsen.io/consistency/models/strict-serializable)
* [Example of serializable but not linearizable schedule](https://gist.github.com/pbailis/8279494)
* [Atomic clocks and distributed databases](https://www.cockroachlabs.com/blog/living-without-atomic-clocks/)
* [Another cockroach article about consistency](https://www.cockroachlabs.com/blog/consistency-model/)
* [Abadi blog](http://dbmsmusings.blogspot.com/)
* [Peter Beilis blog](http://www.bailis.org/blog) (both wrote lots of material on the subject)

### Strict Serializability
I am not a database expert. Here is a [very nice diagram](https://jepsen.io/consistency) showing how various consistency models relate.

Single node Redis is strictly serializable because:
* It’s linearizable - all its single key operations are atomic and once they are committed everyone will immediately see their effects.
* All transactions and multi-key operations are done also atomically in a single thread, so it’s obviously serializable.

More formally: following definition from https://jepsen.io/consistency/models/strict-serializable - due to single threaded design of Redis, its transactions are executed in a global order, which is consistent with the main thread clock, hence it’s strictly serializable.

Serializability is a global property that given a transaction log, there is an order with which transactions are consistent (the log order is not relevant).

Example of serializable but not linearizable transaction: https://gist.github.com/pbailis/8279494

### BLPOP spec

BLPOP key1 key2 key3 0

*BLPOP is a blocking list pop primitive. It is the blocking version of LPOP because it blocks the client connection when there are no elements to pop from any of the given lists. An element is popped from the head of the first list that is non-empty, with the given keys being checked in the order that they are given.*

### Non-blocking behavior of BLPOP
When BLPOP is called, if at least one of the specified keys contains a non-empty list, an element is popped from the head of the list and returned to the caller together with the key it was popped from. Keys are checked in the order that they are given. Let's say that the key1 doesn't exist and key2 and key3 hold non-empty lists. Therefore, in the example above, BLPOP returns the element from list2.

### Blocking behavior
If none of the specified keys exist, BLPOP blocks the connection until another client performs a LPUSH or RPUSH operation against one of the keys. Once new data is present on one of the lists, the client returns with the name of the key unblocking it and the popped value.

### Ordering semantics
If a client tries to wait on multiple keys, but at least one key contains elements, the returned key / element pair is the first key from left to right that has one or more elements. In this case the client will not be blocked. So for instance, BLPOP key1 key2 key3 key4 0, assuming that both key2 and key4 are non-empty, will always return an element from key2.

If multiple clients are blocked for the same key, the first client to be served is the one that was waiting longer (the first that was blocked for the key). Once a client is unblocked it does not retain any priority, when it blocks again with the next call to BLPOP, it will be served accordingly to the queue order of clients already waiting for the same key.

When a client is blocking for multiple keys at the same time, and elements are available at the same time in multiple keys (because of a transaction), the client will be unblocked with the first key on the left that received data via push operation (assuming it has enough elements to serve our client, as there could be earlier clients waiting for this key as well).

### BLPOP and transactions
If multiple elements are pushed either via a transaction or via variadic arguments of LPUSH command then BLPOP is waked after that transaction or command completely finished. Specifically, when a client performs
`LPUSH listkey a b c`, then `BLPOP listkey 0` will pop `c`, because `lpush` pushes first `a`, then `b` and then `c` which will be the first one on the left.

If a client executes a transaction that first pushes into a list and then pops from it atomically, then another client blocked on `BLPOP` won’t pop anything, because it waits for the transaction to finish. When BLPOP itself is run in a transaction its blocking behavior is disabled and it returns the “timed-out” response if there is no element to pop.

### Complexity of implementing BLPOP in Dragonfly
The ordering semantics of BLPOP assume total order of underlying operations. BLPOP must “observe” multiple keys simultaneously in order to determine which one is non-empty in left-to-right order. If there are no keys with lists, BLPOP blocks, waits, and “observes” which key is being filled first.

For the single-threaded Redis the order is determined by following the natural execution of operations inside the main execution thread.  However, for a multi-threaded, shared-nothing execution, there is no concept of total order or a global synchronized timeline. For non-blockign scenario, "observing" keys is atomic because we lock the keys when executing a command in Dragonfly.

However with blocking scenario for BLPOP, we do not have a built-in mechanism to determine which key was filled earlier - since as I said the concept of total order does not exist for multiple shards.

### Interesing examples to consider:

**Ex1:**
```
client1: blpop X, Y  // blocks
client2: lpush X A
client3: exist X Y
```

Client3 should always return 0.

**Ex2:**

```
client1: BLPOP X Y Z
client2: RPUSH X A
client3: RPUSH X B;  RPUSH Y B
```

**Ex3:**

```
client1: BLPOP X Y Z
client2: RPUSH Z C
client3: RPUSH X A
client4: RPUSH X B; RPUSH Y B
```

### BLPOP Ramblings
There are two cases of how a key can appear and wake a blocking `BLPOP`:

a. with lpush/rpush/rename commands.
b. via multi-transaction.

`(a)` is actually easy to reason about, because those commands operate on a single key and single key operations are strictly serializable in shared-nothing architecture.

With `(b)` we need to consider the case where we have "BLPOP X Y 0" and then a multi-transaction fills both `y` and `x` using multiple "lpush" commands. Luckily, a multi-transaction in Dragonfly introduces a global barrier across all its shards, and it does not allow any other transactions to run as long as it does not finish. So the blocking "blpop" won't be awaken until the multi-transaction won't finish its run. By that time the state of the keys will be well defined and "blpop" will be able to choose the first non empty key to pop from.
