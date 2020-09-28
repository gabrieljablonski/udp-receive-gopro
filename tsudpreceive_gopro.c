/*
 * Copyright (C) 2008-2013, Lorenzo Pallara l.pallara@avalpa.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#define MULTICAST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#define UDP_MAXIMUM_SIZE 65535 /* theoretical maximum size */
#define GOPRO_BUFFER_SIZE 15
#define GOPRO_PACKET_SIZE 1500

#define max(a, b) a > b? a : b;

struct GoProPacket {
    int sequence_number;
    int ssrc;
    int len;
    unsigned char packet[GOPRO_PACKET_SIZE];
} gopro_buffer[GOPRO_BUFFER_SIZE];

uint8_t buffered = 0;

void clear_buffer() {
    for (int i = 0; i < GOPRO_BUFFER_SIZE; i++) {
        gopro_buffer[i].sequence_number = -1;
    }
    buffered = 0;
}

int main(int argc, char* argv[]) {

    int sockfd;
    struct sockaddr_in addr;
    #ifdef ip_mreqn
     struct ip_mreqn mgroup;XXX
    #else
     /* according to
          http://lists.freebsd.org/pipermail/freebsd-current/2007-December/081080.html
        in bsd it is also possible to simply use ip_mreq instead of ip_mreqn
        (same as in Linux), so we are using this instead
     */
     struct ip_mreq mgroup;
    #endif
    int reuse;
    unsigned int addrlen;
    int len;
    unsigned char udp_packet[UDP_MAXIMUM_SIZE];

    if (argc != 3) {
        fprintf(stderr, "Usage: %s ip_addr port > output.ts\n", argv[0]);
        return 0;
    } else {
        memset((char *) &mgroup, 0, sizeof(mgroup));
        mgroup.imr_multiaddr.s_addr = inet_addr(argv[1]);
            #ifdef ip_mreqn
        mgroup.imr_address.s_addr = INADDR_ANY;
            #else
            /* this is called 'interface' here */
        mgroup.imr_interface.s_addr = INADDR_ANY;
            #endif
        memset((char *) &addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(atoi(argv[2]));
        addr.sin_addr.s_addr = inet_addr(argv[1]);
        addrlen = sizeof(addr);
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket(): error ");
        return 0;
    }
    
    reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
	    perror("setsockopt() SO_REUSEADDR: error ");
    }

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
	    perror("bind(): error");
        //	close(sockfd);
        //	return 0;
    }
    
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mgroup, sizeof(mgroup)) < 0) {
	    perror("setsockopt() IPPROTO_IP: error ");
    //	close(sockfd);
    //	return 0;
    }

    int prebuffer = 0;
    int prebuffered = 0;
    int clear = 0;
    int turnover = -1;
    int mandar_sn = -1;
    int mandar_ssrc = -1;
    int n = 1024 * 1024 * 1024;
    unsigned int ssrc, last_ssrc;

	int rtpsn;

	setsockopt(sockfd,SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));

    clear_buffer();

    while(1) {
        while(buffered && !prebuffer) {
            int sent = 0;
            for(int i = 0; i < GOPRO_BUFFER_SIZE; ++i) {
                if (mandar_sn == gopro_buffer[i].sequence_number && mandar_ssrc == gopro_buffer[i].ssrc) {
                    // fprintf(stderr, "sending buffered packet %d %d\n", gopro_buffer[i].sequence_number, gopro_buffer[i].ssrc);
                    write(STDOUT_FILENO, gopro_buffer[i].packet, gopro_buffer[i].len);
                    gopro_buffer[i].sequence_number = -1;
                    --buffered;
                    if(mandar_ssrc == turnover) {
                        ++mandar_sn;
                        if(mandar_sn == 256) mandar_sn = 0;
                        mandar_ssrc = 0;
                        turnover = -1;
                    }
                    else 
                        ++mandar_ssrc;
                    sent = 1;
                }
                if (!buffered)
                    break;
            }
            if (!sent) {
                if (prebuffered) {
                    ++mandar_sn;
                    if(mandar_sn == 256) mandar_sn = 0;
                    mandar_ssrc = 0;
                    prebuffered = 0;
                }
                else
                    break;
            }
        }

	    len = recvfrom(sockfd, udp_packet, UDP_MAXIMUM_SIZE, 0, (struct sockaddr *) &addr,&addrlen);
        if (len < 0) {
            perror("recvfrom(): error ");
            continue;
        }

        if(len % 188 == 0) {
            write(STDOUT_FILENO, udp_packet, len);
            continue;
        }

        rtpsn = udp_packet[2] << 8;
        rtpsn |= udp_packet[3];

        ssrc=udp_packet[9];
        ssrc|=udp_packet[8] << 8;

        if(clear) {
            clear_buffer();
            mandar_sn = rtpsn;
            mandar_ssrc = ssrc;
            clear = 0;
        }
    
        if(mandar_sn == -1)
            mandar_sn = rtpsn;

        if(mandar_ssrc == -1)
            mandar_ssrc = ssrc;

        if(!ssrc) {
            if(!buffered)
                prebuffer = 1;
            else
                turnover = last_ssrc;
        }

        if(rtpsn != mandar_sn && !buffered && !prebuffer)
            mandar_sn = rtpsn;

        last_ssrc = ssrc;

        if(!prebuffer && rtpsn == mandar_sn && ssrc == mandar_ssrc) {
            write(STDOUT_FILENO, udp_packet+12, len-12);
            // fprintf(stderr, "wrote packet %d %d %d %d (%d in buffer)\n", rtpsn, ssrc, mandar_sn, mandar_ssrc, buffered);
            mandar_ssrc++;
            continue;
        }
    
        // fprintf(stderr, "buffering packet %d %d %d %d (%d in buffer)\n", rtpsn, ssrc, mandar_sn, mandar_ssrc, buffered);

        for (int i = 0; i < GOPRO_BUFFER_SIZE; i++) {
            if (gopro_buffer[i].sequence_number == -1) {
                gopro_buffer[i].sequence_number = rtpsn;
                gopro_buffer[i].ssrc = ssrc;
                gopro_buffer[i].len = len - 12;
                memcpy(gopro_buffer[i].packet, udp_packet + 12, len - 12);
                ++buffered;
                break;
            }
        }
            
        if (buffered >= GOPRO_BUFFER_SIZE) {
            if(prebuffer) {
                prebuffer = 0;
                prebuffered = 1;
                continue;
            }

            fprintf(stderr, "buffer full %d %d %d %d\n", rtpsn, ssrc, mandar_sn, mandar_ssrc);
            if(mandar_ssrc == turnover) {
                ++mandar_sn;
                if(mandar_sn == 256) mandar_sn = 0;
                mandar_ssrc = 0;
                turnover = -1;
            }
            else 
                ++mandar_ssrc;
            clear = 1;
            // exit(-1);
        }
    }
    
}