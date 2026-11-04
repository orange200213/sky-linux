# Bug: inbound TCP over wlan0 stalls after the first reply

Status: open, kernel/driver side. Rootfs-side work done, real fix not found.

## Symptom

`ssh root@192.168.1.186` (the device's WLAN address, from another machine on the
LAN) never completes. It hangs, it is not refused. ping works, and so does the
TCP connect.

```
$ ssh -vvv root@192.168.1.186
debug1: Remote protocol version 2.0, remote software version OpenSSH_9.9
debug1: SSH2_MSG_KEXINIT sent
debug1: SSH2_MSG_KEXINIT received
debug1: kex: algorithm: sntrup761x25519-sha512@openssh.com
debug1: expecting SSH2_MSG_KEX_ECDH_REPLY      <-- hangs here
```

## Evidence

Same sshd, same host keys, same `/etc/ssh`: over `usb0` (192.168.7.1) it works
every time, over `wlan0` it never does.

sshd's own debug (`-ddd` on a probe instance on :2222) shows it receives the
client's KEXINIT, sends its own, and then waits forever:

```
debug2: peer client KEXINIT proposal [preauth]
debug1: kex: algorithm: sntrup761x25519-sha512@openssh.com [preauth]
debug1: expecting SSH2_MSG_KEX_ECDH_INIT [preauth]
```

So the client's `KEX_ECDH_INIT` never arrives, while the client is waiting for
the reply to it. Host -> device TCP data stops.

`tcpdump -i wlan0 'host 192.168.1.174 and tcp port 22'` on the device:

```
.174 > .186: [S]                      mss 1460
.186 > .174: [S.]                     mss 1460
.174 > .186: [.] ack 1
.174 > .186: [P.] seq 1:44 length 43  SSH-2.0-OpenSSH_9.6p1     <- banner, arrives
.174 > .186: [P.] seq 1:44 length 43  (retransmit, TS +217ms)
.174 > .186: [P.] seq 1:44 length 43  (retransmit, TS +449ms)
.186 > .174: [.] ack 44               <- ACK only after the 3rd try
.186 > .174: [P.] seq 1:27 length 26  <- server banner
.186 > .174: [R.] seq 27, ack 44      <- reset
```

The device ACKs ~450 ms late (802.11 retransmissions of the client's frame are
visible on the device side), and then everything stalls.

## What it is not

| checked | result |
|---|---|
| sshd config / host keys / users | fine - the same sshd serves usb0 perfectly |
| MTU / segment size | MTU 1400 (MSS 1360) fails, MTU 552 (MSS 512) gets further (host key verified, i.e. the KEX succeeds) but auth data still stalls |
| KEX algorithm | curve25519 (tiny client init) also fails |
| GRO / GSO / TSO | disabling them changes nothing |
| memory / OOM | 900 MB available, no OOM in dmesg, no `InCsumErrors` |
| AP / RF quality | RSSI -50, 433 Mbps, 5 GHz 80 MHz |

Direction matters: **outbound** TCP over wlan0 works (`apk`, `wget https://...`
with large transfers complete), and so do ICMP, large ICMP (1400 B with DF,
3000 B fragmented) and the TCP three-way handshake of **inbound** connections.
It is inbound TCP *payload* that stops.

## Fixed on the way

Power save was on, and it cost this link ~50x its latency:

```
before: iw dev wlan0 get power_save -> on    ping 92..205 ms, lossy
after : iw dev wlan0 set power_save off      ping  4.07..4.16 ms, 0% loss
```

That is now done by `/usr/local/sbin/sky-wifi-load.sh` right after `wlan0` comes
up. It did not fix the stall, but it removed a real problem on its own (and the
same knob is worth remembering for throughput).

## Workaround

Use the USB link for shell access (`ssh root@192.168.7.1`); the WLAN is fine for
anything the device initiates, which is all the normal use (DHCP, `apk`, NTP).

## Where to look next

This smells like the qcacld/icnss2 RX path (or its NAPI/aggregation handling):
frames are seen by the driver, the stack ACKs them late, and after the device's
own TX burst nothing further from the peer is delivered to the socket. Next
things worth trying are the RX ring / BA (block-ack) handling in the port and
whether an iperf3 run in the server role (`iperf3 -s` on the device, client on
the LAN) reproduces it without sshd in the picture.
