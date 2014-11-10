#include "main.h"
#define MAX_CLT_TX_BURST 16
#define MAX_CLT_RX_BURST 16

void run_client(int client_id, int *ht_log, struct rte_mempool **l2fwd_pktmbuf_pool)
{
	// [xia-router0 - xge0,1,2,3], [xia-router1 - xge0,1,2,3]
	LL src_mac_arr[2][4] = {{0x36d3bd211b00, 0x37d3bd211b00, 0xa8d6a3211b00, 0xa9d6a3211b00},
							{0x44d7a3211b00, 0x45d713211b00, 0x0ad7a3211b00, 0x0bd7a3211b00}};

	// [xia-router2 - xge0,1,4,5], [xia-router2 - xge2,3,6,7]
	LL dst_mac_arr[2][4] = {{0x6c10bb211b00, 0x6d10bb211b00, 0xc8a610ca0568, 0xc9a610ca0568},
							{0x64d2bd211b00, 0x65d2bd211b00, 0xa2a610ca0568, 0xa3a610ca0568}};

	// Even cores take xge0,1. Odd cores take xge2, xge3
	int lcore_to_port[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

	int i;

	struct rte_mbuf *rx_pkts_burst[MAX_CLT_RX_BURST], *tx_pkts_burst[MAX_CLT_TX_BURST];

	int lcore_id = rte_lcore_id();

	int port_id = lcore_to_port[lcore_id];
	if(!ISSET(XIA_R0_PORT_MASK, port_id)) {
		red_printf("Lcore %d uses disabled port (port %d). Exiting.\n", lcore_id, port_id);
		exit(-1);
	}

	// This is a valid queue_id because all client ports have 3 queues
	int queue_id = lcore_id % 3;
	red_printf("Client: lcore: %d, port: %d, queue: %d\n", lcore_id, port_id, queue_id);

	LL prev_tsc = 0, cur_tsc = 0;
	prev_tsc = rte_rdtsc();

	LL nb_tx = 0, nb_rx = 0, nb_fails = 0;
	struct ether_hdr *eth_hdr;
	struct ipv4_hdr *ip_hdr;
	uint8_t *src_mac_ptr, *dst_mac_ptr;

	LL rx_samples = 0, latency_tot = 0;
	uint64_t rss_seed = 0xdeadbeef;

	// sizeof(ether_hdr) + sizeof(ipv4_hdr) is 34 --> 36 for 4 byte alignment
	int hdr_size = 36;

        printf("The client send-wait loop...\n");
	while (1) {

		for(i = 0; i < MAX_CLT_TX_BURST; i ++) {
			tx_pkts_burst[i] = rte_pktmbuf_alloc(l2fwd_pktmbuf_pool[lcore_id]);
			CPE(tx_pkts_burst[i] == NULL, "tx_alloc failed\n");
			
			eth_hdr = rte_pktmbuf_mtod(tx_pkts_burst[i], struct ether_hdr *);
			ip_hdr = (struct ipv4_hdr *) ((char *) eth_hdr + sizeof(struct ether_hdr));
		
			src_mac_ptr = &eth_hdr->s_addr.addr_bytes[0];
			if((fastrand(&rss_seed) & 0xff) == 0) {
				set_mac(src_mac_ptr, src_mac_arr[client_id][port_id]);
			} else {
				set_mac(src_mac_ptr, 0xdeadbeef);
			}

			dst_mac_ptr = &eth_hdr->d_addr.addr_bytes[0];
			set_mac(dst_mac_ptr, dst_mac_arr[client_id][port_id]);

			eth_hdr->ether_type = htons(ETHER_TYPE_IPv4);
	
			// These 3 fields of ip_hdr are required for RSS
			ip_hdr->src_addr = fastrand(&rss_seed);
			ip_hdr->dst_addr = fastrand(&rss_seed);
			ip_hdr->version_ihl = 0x40 | 0x05;

			tx_pkts_burst[i]->pkt.nb_segs = 1;
			tx_pkts_burst[i]->pkt.pkt_len = 60;
			tx_pkts_burst[i]->pkt.data_len = 60;

			// Add request, lcore_id, and timestamp
			int *req = (int *) (rte_pktmbuf_mtod(tx_pkts_burst[i], char *) + hdr_size);
			req[0] = lcore_id;							// 36 -> 40
			req[1] = fastrand(&rss_seed) & LOG_CAP_;	// 40 -> 44
			// Bytes 44 -> 48 are reserved for response (req[2])
			
			LL *tsc = (LL *) (rte_pktmbuf_mtod(tx_pkts_burst[i], char *) + hdr_size + 12);
			tsc[0] = rte_rdtsc();		// 48 -> 56
		}

		int nb_tx_new = rte_eth_tx_burst(port_id, queue_id, tx_pkts_burst, MAX_CLT_TX_BURST);
		nb_tx += nb_tx_new;
		for(i = nb_tx_new; i < MAX_CLT_TX_BURST; i++) {
			rte_pktmbuf_free(tx_pkts_burst[i]);
		}

		micro_sleep(2, C_FAC);

		// RX drain
		while(1) {
			int nb_rx_new = rte_eth_rx_burst(port_id, queue_id, rx_pkts_burst, MAX_CLT_RX_BURST);
			if(nb_rx_new == 0) {
				break;
			}

			nb_rx += nb_rx_new;
			for(i = 0; i < nb_rx_new; i ++) {
				// Verify the server's response
				int *req = (int *) (rte_pktmbuf_mtod(rx_pkts_burst[i], char *) + hdr_size);
				int req_addr = req[1];
				int resp = req[2];

				int acc_i;		// mem-access iterator
				for(acc_i = 0; acc_i < NUM_ACCESSES; acc_i ++) {
					req_addr = ht_log[req_addr];
				}
				if(req_addr != resp) {
					nb_fails ++;
				}

				// Retrive send-timestamp and lcore from which this pkt was sent
				LL *tsc = (LL *) (rte_pktmbuf_mtod(rx_pkts_burst[i], char *) + hdr_size + 12);
				int tx_lcore = req[0];		// The lcore that sent this packet to the server
				if(lcore_id == tx_lcore) {
					rx_samples ++;
					LL cur_tsc = rte_rdtsc();
					latency_tot += (cur_tsc - tsc[0]);
				}

				rte_pktmbuf_free(rx_pkts_burst[i]);
			}
		}

		// Print TX stats : because clients rarely process RX pkts
		if (unlikely(nb_tx >= 10000000)) {
			cur_tsc = rte_rdtsc();
			double nanoseconds = C_FAC * (cur_tsc - prev_tsc);
			prev_tsc = cur_tsc;

			printf("Lcore = %d, TX per sec = %f, Avg. latency = %.2f us | Fails = %lld, nb_rx = %lld\n",
				lcore_id, nb_tx / (nanoseconds / GHZ_CPS),
				(C_FAC * (latency_tot / (rx_samples + .01))) / 1000, nb_fails, nb_rx);
			
			nb_tx = 0;

			nb_fails = 0;
			rx_samples = 0;
			latency_tot = 0;
		}
	}
}
