## Update: diagnosing AES67 packet loss on receive

**Symptom**: A tcpdump on this decoder box's `eth0` for `dst 239.69.1.1 and udp port 5004` over 8s showed only **~299 pps** (2390 packets), but the sender is confirmed transmitting at **~987 pps** — roughly 70% of packets are being lost somewhere before/at this box. Expected rate is ~1000 pps (1ms packet time, L24/48000/1 mono AES67 stream).

**Confirmed on the sender side** (aes67 box, do not need to re-check):
- `ptp4l`/`phc2sys` grandmaster active, all PipeWire/jltcgen services active and linked
- Live capture on the sender's own `eth0` shows a clean, correct ~987 pps on `239.69.1.1:5004` — the stream leaving the sender is healthy. The loss is happening in the network path or on this box, not at the source.

**Ruled out**:
- Wi-Fi in the path — both ends are wired.
- IGMP snooping/querier misconfiguration — it's an unmanaged home LAN switch, which just floods multicast to every port with no filtering, so snooping can't be the cause.
- An L3 hop dropping the sender's `net.ttl=1` packets — considered unlikely (flat single-switch LAN).

**Still to check on this decoder box** — please run these and report results:

```bash
ip -br link                          # confirm eth0 is the interface actually in use
ip maddr show eth0                   # is 239.69.1.1 actually joined?
ip -s -s link show eth0              # rx errors/drops/missed (note: -s -s for extended stats)
ethtool eth0 | grep -i speed         # confirm negotiated 1000Mb/s full-duplex, not 100Mb/half-duplex
ss -u -a -n | grep 5004              # Recv-Q backlog = downstream (our code) can't keep up with incoming rate
dmesg -T | tail -50                  # NIC driver errors/resets around the time of the capture
```

Likely remaining causes to investigate based on results: NIC-level drops/errors, a duplex/speed mismatch, or the receiving software (PipeWire's `rtp-source` / whatever is consuming the socket) falling behind and dropping packets internally rather than genuine network loss. Report back what these show and we'll narrow it from there.
