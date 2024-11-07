#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>    // 네트워크 주소 함수
#include <sys/socket.h>   // 소켓 프로그래밍
#include <unistd.h>       // usleep 함수
#include <time.h>         // 시간 관련 함수
#include <stdint.h>       // 고정 폭 정수 타입
#include <alsa/asoundlib.h> // ALSA 오디오 함수, linux에서만 사용 가능

// 컴파일 : gcc -o audiocapture audiocapture.c -lasound
// 수신 측 : ffmpeg -i rtp://127.0.0.1:5004 -f alsa default

#define DEST_IP "127.0.0.1"  // 라즈베리파이의 IP 주소 
#define RTP_PORT 5004 // rtp는 짝수번호
#define RTCP_PORT 5005 // rtp 포트번호 + 1
#define PAYLOAD_SIZE 160  // 20ms of G.711 audio at 8kHz

// RTP header structure
struct rtp_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN // 리틀 엔디안 시스템을 위한 비트 필드 정의
    uint8_t cc:4;
    uint8_t x:1;
    uint8_t p:1;
    uint8_t version:2;
    uint8_t pt:7;
    uint8_t m:1;
#elif __BYTE_ORDER == __BIG_ENDIAN // 빅 엔디안 시스템을 위한 비트 필드 정의
    uint8_t version:2;
    uint8_t p:1;
    uint8_t x:1;
    uint8_t cc:4;
    uint8_t m:1;
    uint8_t pt:7;
#else
#error "Please fix <bits/endian.h>"
#endif
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed)); // 구조체 패딩 방지 (펭귄 교재 481, 482 페이지 참고)

// RTCP SR (Sender Report) 구조
struct rtcp_sr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t rc:5;
    uint8_t p:1;
    uint8_t version:2;
#elif __BYTE_ORDER == __BIG_ENDIAN
    uint8_t version:2;
    uint8_t p:1;
    uint8_t rc:5;
#else
#error "Please fix <bits/endian.h>"
#endif
    uint8_t pt;          // 패킷 타입 (200 for SR)
    uint16_t length;     // RTCP 패킷의 길이
    uint32_t ssrc;       // 송신자의 SSRC
    uint32_t ntp_timestamp_msw;  // NTP 타임스탬프 상위 32비트
    uint32_t ntp_timestamp_lsw;  // NTP 타임스탬프 하위 32비트
    uint32_t rtp_timestamp;      // RTP 타임스탬프
    uint32_t sender_packet_count; // 송신한 패킷 수
    uint32_t sender_octet_count;  // 송신한 옥텟(바이트) 수
} __attribute__((packed));

// RTP 헤더 생성 함수
void create_rtp_header(struct rtp_header *header, unsigned short seq_num, unsigned int timestamp, unsigned int ssrc) {
    header->version = 2;
    header->p = 0;
    header->x = 0;
    header->cc = 0;
    header->m = 0;
    header->pt = 0;  // 0 for PCMU (G.711 µ-law)
    header->seq_num = htons(seq_num);
    header->timestamp = htonl(timestamp);
    header->ssrc = htonl(ssrc);
}

// RTCP SR(Sender Report) 생성 함수
void create_rtcp_sr(struct rtcp_sr *sr, uint32_t ssrc, uint32_t rtp_timestamp, uint32_t packet_count, uint32_t octet_count) {
    memset(sr, 0, sizeof(*sr));
    sr->version = 2;
    sr->p = 0;
    sr->rc = 0;
    sr->pt = 200;  // 200 for Sender Report
    sr->length = htons(6);  // 6 32-bit words
    sr->ssrc = htonl(ssrc);
    
    // 현재 시간을 NTP 형식으로 변환
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t ntp_time = ((uint64_t)(now.tv_sec + 2208988800ULL) << 32) | 
                        ((uint64_t)now.tv_nsec * 0x100000000ULL / 1000000000ULL);
    
    sr->ntp_timestamp_msw = htonl((uint32_t)(ntp_time >> 32));
    sr->ntp_timestamp_lsw = htonl((uint32_t)(ntp_time & 0xFFFFFFFF));
    sr->rtp_timestamp = htonl(rtp_timestamp);
    sr->sender_packet_count = htonl(packet_count);
    sr->sender_octet_count = htonl(octet_count);
}

// G.711 µ-law 인코딩 함수
unsigned char g711_ulaw(int sample) {
    const int MAX = 32767; // PCM 오디오 샘플의 최대 값
    const int BIAS = 0x84; // 인코딩에서 사용하는 바이어스 값, 압축시 왜곡을 줄이기 위해 사용
    int sign = (sample >> 8) & 0x80; // 샘플의 상위 8비트 추출 (부호 확인)
    if (sign != 0) sample = -sample; // 음수일 경우 양수로 변환
    if (sample > MAX) sample = MAX; // 최대값 제한
    sample += BIAS; // 바이어스 추가: 작은 신호 왜곡을 줄이기 위함
    int exponent = 7;  // 지수(exponent) 계산
    int mask;
    for (; exponent > 0; exponent--) {
        mask = 1 << (exponent + 3);
        if (sample >= mask) break;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F; // 가수 계산 (하위 4비트)
    return ~(sign | (exponent << 4) | mantissa);  // 부호, 지수, 가수를 결합하여 최종 인코딩 값 생성
}

int main() {
    int rtp_sockfd, rtcp_sockfd;
    struct sockaddr_in rtp_dest_addr, rtcp_dest_addr;
    struct rtp_header rtp_hdr;
    struct rtcp_sr rtcp_sr;
    unsigned char payload[PAYLOAD_SIZE];
    unsigned char rtp_packet[sizeof(struct rtp_header) + PAYLOAD_SIZE];
    
    // 난수 시드 초기화 및 초기 시퀀스 번호 설정
    srand((unsigned int)time(NULL));
    unsigned short seq_num = rand() % 65535;
    unsigned int timestamp = 0;
    unsigned int ssrc = 12345;
    unsigned int packet_count = 0;
    unsigned int octet_count = 0;

    // ALSA 초기화
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    int dir;
    unsigned int sample_rate = 8000;  // G.711의 샘플링 레이트
    int rc; // ALSA 오디오 파라미터 적용

    // ALSA PCM 디바이스 열기
    rc = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(rc));
        exit(1);
    }

    // ALSA PCM 하드웨어 파라미터 설정
    snd_pcm_hw_params_alloca(&params); // 오디오 디바이스의 매개변수 설정을 위한 공간 확보
    snd_pcm_hw_params_any(pcm_handle, params); // 기본값으로 초기화
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED); // 인터리브드 모드로 설정
    snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE); // 오디오 포맷 16비트로 설정
    snd_pcm_hw_params_set_channels(pcm_handle, params, 1);  // 모노로 설정
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, &sample_rate, &dir); // 샘플링 레이트 설정
    rc = snd_pcm_hw_params(pcm_handle, params);
    if (rc < 0) {
        fprintf(stderr, "Unable to set HW parameters: %s\n", snd_strerror(rc));
        exit(1);
    }

    // 버퍼 크기 설정 (20ms에 해당하는 샘플 수)
    int frames = PAYLOAD_SIZE;  // G.711은 8kHz에서 20ms당 160 샘플
    short buffer[PAYLOAD_SIZE];
    
    // RTP 소켓 생성
    rtp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sockfd < 0) {
        perror("RTP socket creation failed");
        exit(1);
    }

    // RTCP 소켓 생성
    rtcp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtcp_sockfd < 0) {
        perror("RTCP socket creation failed");
        exit(1);
    }

    // RTP 목적지 주소 설정
    memset(&rtp_dest_addr, 0, sizeof(rtp_dest_addr));
    rtp_dest_addr.sin_family = AF_INET;
    rtp_dest_addr.sin_port = htons(RTP_PORT);
    inet_pton(AF_INET, DEST_IP, &rtp_dest_addr.sin_addr);

    // RTCP 목적지 주소 설정
    memset(&rtcp_dest_addr, 0, sizeof(rtcp_dest_addr));
    rtcp_dest_addr.sin_family = AF_INET;
    rtcp_dest_addr.sin_port = htons(RTCP_PORT);
    inet_pton(AF_INET, DEST_IP, &rtcp_dest_addr.sin_addr);

    // RTP 패킷 전송 루프
    while (1) {
        // ALSA라이브러리를 통해 오디오 데이터 캡처
        rc = snd_pcm_readi(pcm_handle, buffer, frames);
        if (rc == -EPIPE) { // 오버런일 경우 처리
            fprintf(stderr, "Overrun occurred\n");
            snd_pcm_prepare(pcm_handle);
            continue;
        } else if (rc < 0) { // 에러가 발생했을 때의 처리
            fprintf(stderr, "Error from read: %s\n", snd_strerror(rc));
            continue;
        } else if (rc != frames) { // 요청한 것보다 적은 프레임을 읽었을 때
            fprintf(stderr, "Short read, read %d frames\n", rc);
        }

        // 캡처한 데이터를 G.711 µ-law으로 인코딩
        for (int i = 0; i < PAYLOAD_SIZE; i++) {
            payload[i] = g711_ulaw(buffer[i]);
        }

        // RTP 헤더 생성
        create_rtp_header(&rtp_hdr, seq_num, timestamp, ssrc);

        // RTP 패킷화(Packetizing)
        memcpy(rtp_packet, &rtp_hdr, sizeof(struct rtp_header));
        memcpy(rtp_packet + sizeof(struct rtp_header), payload, PAYLOAD_SIZE);

        // RTP 패킷 전송
        if (sendto(rtp_sockfd, rtp_packet, sizeof(rtp_packet), 0, (struct sockaddr *)&rtp_dest_addr, sizeof(rtp_dest_addr)) < 0) {
            perror("RTP sendto() error");
            exit(1);
        }
        printf("RTP packet %d sent\n", ntohs(rtp_hdr.seq_num));

        // 카운터 업데이트
        seq_num++;
        timestamp += PAYLOAD_SIZE;  // G.711의 경우 8000 Hz, 160 샘플 = 20ms
        packet_count++;
        octet_count += PAYLOAD_SIZE;

        // 50패킷(1초)마다 RTCP SR 전송
        if (packet_count % 50 == 0) {
            create_rtcp_sr(&rtcp_sr, ssrc, timestamp, packet_count, octet_count);
            if (sendto(rtcp_sockfd, &rtcp_sr, sizeof(rtcp_sr), 0, (struct sockaddr *)&rtcp_dest_addr, sizeof(rtcp_dest_addr)) < 0) {
                perror("RTCP SR sendto() error");
                exit(1);
            }
            printf("RTCP SR sent\n");
        }

        usleep(20000);  // 20ms 지연 (50 패킷/초)
    }

    // 소켓 및 ALSA 핸들러 닫기
    close(rtp_sockfd);
    close(rtcp_sockfd);
    snd_pcm_close(pcm_handle);
    return 0;
}
