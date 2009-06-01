/* -*- c++ -*- */
/*
 * Copyright 2008, 2009 Dominic Spill, Michael Ossmann                                                                                            
 * Copyright 2007 Dominic Spill                                                                                                                   
 * Copyright 2005, 2006 Free Software Foundation, Inc.
 * 
 * This file is part of gr-bluetooth
 * 
 * gr-bluetooth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * gr-bluetooth is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with gr-bluetooth; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bluetooth_multi_hopper.h"
#include "bluetooth_packet.h"

/*
 * Create a new instance of bluetooth_multi_hopper and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
bluetooth_multi_hopper_sptr
bluetooth_make_multi_hopper(double sample_rate, double center_freq, double squelch_threshold, int LAP, bool aliased, bool tun)
{
  return bluetooth_multi_hopper_sptr (new bluetooth_multi_hopper(sample_rate, center_freq, squelch_threshold, LAP, aliased, tun));
}

//private constructor
bluetooth_multi_hopper::bluetooth_multi_hopper(double sample_rate, double center_freq, double squelch_threshold, int LAP, bool aliased, bool tun)
  : bluetooth_multi_block(sample_rate, center_freq, squelch_threshold)
{
	d_LAP = LAP;
	d_aliased = aliased;
	d_have_clock6 = false;
	d_have_clock27 = false;
	d_tun = tun;
	set_symbol_history(3125);
	d_piconet = bluetooth_make_piconet(d_LAP);
	printf("lowest channel: %d, highest channel %d\n", d_low_channel, d_high_channel);

	/* Tun interface */
	if(d_tun) {
		chan_name = "gr-bluetooth";

		if((d_tunfd = mktun(chan_name, d_ether_addr)) == -1) {
			fprintf(stderr, "warning: was not able to open TUN device, "
			  "disabling Wireshark interface\n");
			// throw std::runtime_error("cannot open TUN device");
		}
	}
}

//virtual destructor.
bluetooth_multi_hopper::~bluetooth_multi_hopper ()
{
}

int 
bluetooth_multi_hopper::work(int noutput_items,
			       gr_vector_const_void_star &input_items,
			       gr_vector_void_star &output_items)
{
	int retval, channel, num_symbols, latest_ac;
	uint32_t clkn; /* native (local) clock in 625 us */
	char symbols[history()]; //poor estimate but safe
	int num_candidates = -1;

	clkn = (int) (d_cumulative_count / d_samples_per_slot) & 0x7ffffff;

	if (d_have_clock27) {
		/* now that we know the clock and UAP, follow along and sniff each time slot on the correct channel */
		hopalong(input_items, symbols, clkn);
	} else {
		for (channel = d_low_channel; channel <= d_high_channel; channel++)
		{
			num_symbols = channel_symbols(channel, input_items, symbols, history());
	
			if (num_symbols >= 72 )
			{
				/* don't look beyond one slot for ACs */
				latest_ac = (num_symbols - 72) < 625 ? (num_symbols - 72) : 625;
				retval = bluetooth_packet::sniff_ac(symbols, latest_ac);
				if(retval > -1) {
					bluetooth_packet_sptr packet = bluetooth_make_packet(&symbols[retval], num_symbols - retval);
					if (packet->get_LAP() == d_LAP && packet->header_present()) {
						if(!d_have_clock6) {
							/* working on CLK1-6/UAP discovery */
							d_have_clock6 = d_piconet->UAP_from_header(packet, clkn, channel);
							if(d_have_clock6) {
								/* got CLK1-6/UAP, start working on CLK1-27 */
								printf("\nCalculating complete hopping sequence.\n");
								printf("%d initial CLK1-27 candidates\n", d_piconet->init_hop_reversal(d_aliased));
								/* use previously observed packets to eliminate candidates */
								num_candidates = d_piconet->winnow();
								printf("%d CLK1-27 candidates remaining\n", num_candidates);
							}
						} else {
							/* continue working on CLK1-27 */
							/* we need timing information from an additional packet, so run through UAP_from_header() again */
							d_have_clock6 = d_piconet->UAP_from_header(packet, clkn, channel);
							if (!d_have_clock6) {
								reset(); //FIXME piconet should do this
								break;
							}
							num_candidates = d_piconet->winnow();
							printf("%d CLK1-27 candidates remaining\n", num_candidates);
						}
						/* CLK1-27 results */
						if(num_candidates == 1) {
							/* win! */
							printf("\nAcquired CLK1-27 offset = 0x%07x\n", d_piconet->get_offset());
							d_have_clock27 = true;
						} else if(num_candidates == 0) {
							/* fail! */
							reset();
						}
						break;
					}
				}
			}
		}
	}
	d_cumulative_count += (int) d_samples_per_slot;

    	/* 
	 * The runtime system wants to know how many output items we produced, assuming that this is equal
	 * to the number of input items consumed.  We tell it that we produced/consumed one time slot of
	 * input items so that our next run starts one slot later.
	 */
	return (int) d_samples_per_slot;
}

/*
 * follow a piconet's hopping sequence and look for packets on the appropriate
 * channel for each time slot
 */
void bluetooth_multi_hopper::hopalong(gr_vector_const_void_star &input_items,
		char *symbols, uint32_t clkn)
{
	int ac_index, channel, num_symbols, latest_ac;
	char observable_channel;
	uint32_t clock27 = (clkn + d_piconet->get_offset()) & 0x7ffffff;
	channel = d_piconet->hop(clock27);
	if (d_aliased)
		observable_channel = d_piconet->aliased_channel(channel);
	else
		observable_channel = channel;
	if (observable_channel >= d_low_channel && observable_channel <= d_high_channel) {
		//FIXME history() + noutput_items?
		num_symbols = channel_symbols(observable_channel, input_items, symbols, history());
		if (num_symbols >= 72 ) {
			latest_ac = (num_symbols - 72) < 625 ? (num_symbols - 72) : 625;
			ac_index = bluetooth_packet::sniff_ac(symbols, latest_ac);
			if(ac_index > -1) {
				bluetooth_packet_sptr packet = bluetooth_make_packet(&symbols[ac_index], num_symbols - ac_index);
				if(packet->get_LAP() == d_LAP) {
					printf("clock 0x%07x, channel %2d: ", clock27, channel);
					if (packet->header_present()) {
						packet->set_UAP(d_piconet->get_UAP());
						packet->set_clock(clock27);
						packet->decode();
						if(packet->got_payload()) {
							packet->print();
							if(d_tun) {
								/* include 3 bytes for packet header */
								int length = packet->get_payload_length() + 3;
								char *data = packet->tun_format();
								int addr = (packet->get_UAP() << 24) | packet->get_LAP();
								write_interface(d_tunfd, (unsigned char *)data, length, 0, addr, ETHER_TYPE);
							}
						}
					} else {
						printf("ID\n");
						if(d_tun) {
							int addr = (d_piconet->get_UAP() << 24) | packet->get_LAP();
							write_interface(d_tunfd, NULL, 0, 0, addr, ETHER_TYPE);
						}
					}

				}
			}
		}
	}
}

/*
 * start everything over, even CLK1-6/UAP discovery, because we can't trust
 * what we have
 */
void bluetooth_multi_hopper::reset()
{
	printf("Failed to acquire clock. starting over . . .\n\n");
	d_piconet->reset();
	d_have_clock6 = false;
}
