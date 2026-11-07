# A wheel-node is down, suspect or dead

**What customers feel:** usually nothing. With N = 3 and W = R = 2, every key keeps two live
replicas when one node is gone. Sloppy quorum sends writes to a stand-in, which holds a hint.
Requests that were routed to the node in the ~3 s before it was declared dead time out and are
retried by clients (illustrated/03 §5).

**Look first**

1. Which node, and who thinks so:
   `wheelctl -t <any live node> ring`. Each node reports its own failure detector's view.
   If only one observer sees a peer as dead, suspect the link between them, not the peer.
2. Is the process alive? `wheelctl -t <that node> health`, or its `up` series in Prometheus.
   No answer means a crash or an unreachable host. An answer while peers call it dead means a
   partition.
3. The Grafana row *CartwheelKV*: hints pending should rise on the peers while the node is out.

**Actions**

- A crash: restart it. Its incarnation rises, peers accept it within one gossip round, and hinted
  handoff drains the backlog (watch *Hints pending* fall to 0).
- A partition: fix the network. Do not wipe the node's data. Its writes from before are still
  valid and anti-entropy reconciles them.
- Replacing a node with an empty disk is safe. Read repair and anti-entropy refill it, slowly,
  because joins do not stream data yet (illustrated/03 §7).
- Do not lower `dead_after` during an incident. It shortens the error window but turns slow
  nodes into "dead" ones.
