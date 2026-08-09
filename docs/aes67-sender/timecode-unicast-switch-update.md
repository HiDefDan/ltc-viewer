## Update: switched to unicast for testing (sender-side changes applied)

Following the root-cause you found (LAN switch enforces a ~300pps multicast ceiling,
starving both the 987pps AES67 stream and PTP multicast) — both PTP and the AES67
stream have been switched to unicast on this sender box until the managed switch
arrives. Here's exactly what changed, so you can configure/verify the matching side.

### 1. PTP: unicast negotiation enabled on the grandmaster

`unicast_listen 1` is now set in `/etc/linuxptp/ptp4l.conf` on the sender (10.0.23.207).
This is additive — multicast Announce/Sync are untouched, the master now *also* grants
unicast service on request. Domain stays **0**, priority1 stays **1**, grandmaster
identity is still `D8-3A-DD-FF-FE-BC-84-9B`.

Configure your `unicast_master_table` to point at `10.0.23.207` and confirm negotiation
succeeds — check with `pmc -u -b 0 'GET PARENT_DATA_SET'` on this decoder box; you
should see the sender's grandmasterIdentity as parent, and `GET CURRENT_DATA_SET`
should show a converging/near-zero `offsetFromMaster`.

**Also worth knowing**: on restart, `ptp4l` on the sender turned out to have been sitting
in a `FAULTY` port state since its last reboot (~19 min, unrelated to the switch —
a `SIOCSHWTSTAMP` ioctl race at boot). It's fixed now and hardened against recurring
(added a `network-online.target` dependency), but flagging it in case it explains any
earlier "no PTP at all" observations on your side that you'd otherwise have attributed
solely to the switch.

### 2. AES67 stream: retargeted to unicast

**Was**: multicast `239.69.1.1:5004`
**Now**: unicast `10.0.23.205:5004` (this decoder box's address)

Everything else is unchanged: `L24/48000/1` (24-bit BE PCM, 48kHz, mono), RTP payload
type `127`, `1ms` packet time (156-byte UDP packets: 12B RTP header + 144B = 48
samples × 3B).

Config mechanism (for your records): sender's
`~/.config/pipewire/pipewire.conf.d/10-aes67-ltc-rtp.conf`,
`libpipewire-module-rtp-sink` module args — the multicast `destination.ip` line was
commented out (not deleted, for easy revert) and replaced with the unicast one:
```
#destination.ip     = "239.69.1.1"      # multicast — preserved, commented out
destination.ip      = "10.0.23.205"     # unicast test target (active)
destination.port    = 5004               # unchanged
```
`local.ifname = "eth0"`, `net.ttl = 1`, and all `sess.*`/`audio.*` settings are
untouched. Note `libpipewire-module-rtp-sap` is still loaded on the sender but is
inert now (SAP is multicast-only) — irrelevant for unicast, no action needed on your
end regarding it.

**Verified from the sender side**: live capture confirms ~979 pps flowing to
`10.0.23.205:5004`, correct 156-byte packets, matching the 1ms ptime spec. There was a
brief burst of `sendmsg() failed: Connection refused` (ICMP port-unreachable) right at
the sender's restart — expected/harmless if your `rtp-source` wasn't bound to
`:5004` yet at that exact instant; it self-resolved once steady flow started.

### What to check/confirm here

1. Configure `rtp-source` (or whatever's receiving AES67) for **unicast** on `5004`
   instead of joining the `239.69.1.1` multicast group — no group join needed now,
   just listen on your own address.
2. Confirm PTP unicast negotiation completes and offset converges (commands above).
3. Confirm `jltcdump` (or your decode path) sees a clean, gap-free ~1000pps stream and
   decodes correct wall-clock-matching LTC.
4. Report back pps received and PTP offset so we can compare against what we saw
   leaving the sender.

