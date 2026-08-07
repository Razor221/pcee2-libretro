// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The functions DEV9 actually calls on Unix. Unlike the Windows list this is
// deliberately minimal: every entry here has to be resolvable in whatever
// libpcap the host happens to ship, or the adapter is disabled.

#ifdef FUNCTION_SHIM_ANY_ARG
#define FUNCTION_SHIM_1_ARG FUNCTION_SHIM_ANY_ARG
#define FUNCTION_SHIM_2_ARG FUNCTION_SHIM_ANY_ARG
#define FUNCTION_SHIM_3_ARG FUNCTION_SHIM_ANY_ARG
#define FUNCTION_SHIM_5_ARG FUNCTION_SHIM_ANY_ARG
#endif

FUNCTION_SHIM_5_ARG(pcap_t*, pcap_open_live, const char*, int, int, int, char*)
FUNCTION_SHIM_1_ARG(void, pcap_close, pcap_t*)
FUNCTION_SHIM_3_ARG(int, pcap_next_ex, pcap_t*, struct pcap_pkthdr**, const u_char**)
FUNCTION_SHIM_3_ARG(int, pcap_sendpacket, pcap_t*, const u_char*, int)
FUNCTION_SHIM_3_ARG(int, pcap_setnonblock, pcap_t*, int, char*)
FUNCTION_SHIM_2_ARG(int, pcap_setfilter, pcap_t*, struct bpf_program*)
FUNCTION_SHIM_5_ARG(int, pcap_compile, pcap_t*, struct bpf_program*, const char*, int, bpf_u_int32)
FUNCTION_SHIM_1_ARG(void, pcap_freecode, struct bpf_program*)
FUNCTION_SHIM_1_ARG(char*, pcap_geterr, pcap_t*)
FUNCTION_SHIM_1_ARG(int, pcap_datalink, pcap_t*)
FUNCTION_SHIM_1_ARG(const char*, pcap_datalink_val_to_name, int)
FUNCTION_SHIM_2_ARG(int, pcap_findalldevs, pcap_if_t**, char*)
FUNCTION_SHIM_1_ARG(void, pcap_freealldevs, pcap_if_t*)

#undef FUNCTION_SHIM_1_ARG
#undef FUNCTION_SHIM_2_ARG
#undef FUNCTION_SHIM_3_ARG
#undef FUNCTION_SHIM_5_ARG

#ifdef FUNCTION_SHIM_ANY_ARG
#undef FUNCTION_SHIM_ANY_ARG
#endif
