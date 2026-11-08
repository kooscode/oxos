// The kernel's network stack (src/network/, spec/oxos_internal/31_upload_server.md §2): the network card polled, IPv4
// with ARP, ping and DHCP, and a small TCP server. Boot-only: the upload server uses it before the first title starts.
// Everything runs in the caller's thread: NetPoll handles what has arrived and what is due.
#pragma once
#include <xk/types.h>

// --- The stack (network/ip.cpp, network/nic.cpp)

enum NET_STATE : ULONG {
    NetNoLink,  // no cable, or no link partner yet
    NetAsking,  // the link is up; DHCP is asking for an address
    NetLeased,  // an address from DHCP
    NetFixed,   // no DHCP answer within 30 s: the netboot address (NETBOOT_IP)
};

bool NetStart(); // the card and its memory; false without the card or memory
void NetStop();  // the card stopped (no more DMA), its memory and the connections' freed
bool NetPoll();  // received frames, retransmissions, DHCP, the link: call often; true when frames arrived
NET_STATE NetState();
void NetAddressText(char *Text, ULONG Size); // "192.168.1.91" ("" without an address)

// --- TCP (network/tcp.cpp): a server on port 80, NET_TCP_CONNECTIONS connections at a time. A connection is named
// by an id that stays unique after the connection ends, so a stale id is refused rather than reaching a new one.

constexpr ULONG NET_TCP_CONNECTIONS = 4;
LONG NetTcpAccept();                                     // a connection established since the last call, else -1
bool NetTcpOpen(LONG Id);                                // still connected (not reset or closed)
ULONG NetTcpReceive(LONG Id, PVOID Buffer, ULONG Size);  // bytes received so far, up to Size
bool NetTcpPeerClosed(LONG Id);                          // the peer has sent everything (FIN) and it has been read
ULONG NetTcpSend(LONG Id, const void *Data, ULONG Size); // bytes queued (fewer while the send buffer is full)
bool NetTcpAllSent(LONG Id);                             // everything queued has been acknowledged
void NetTcpClose(LONG Id);                               // FIN after the queued data; the connection then ends
