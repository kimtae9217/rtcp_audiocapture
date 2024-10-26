# RTP와 RTCP를 사용하여 음성 캡처 후 전송

## 프로젝트 개요

- **프로젝트 환경**
	- 사용 언어 :  C
	- 개발 환경 :  RaspberryPi 5 , Linux

- **구성 요소**
1. ALSA(Advanced Linux Sound Architecture): 리눅스에서 오디오 캡처를 담당하는 라이브러리
2. G.711 µ-law 인코딩: 오디오 데이터를 압축하는 코덱
3. RTP/RTCP: RTP를 통해 오디오 또는 음성 데이터를 전송하고 RTCP로 SR(Sender Report) 전송

## Architecture
![image](https://github.com/user-attachments/assets/f32762d1-cf66-4241-bc57-8b3f9865d330)

## Manual

- Install
```bash
sudo apt update
sudo apt install libasound2-dev  // ALSA 
sudo apt install ffmpeg		 // ffmpeg 
```

- Complie
```bash
gcc -o audiocapture audiocapture.c -lasound

```

- Start

한 개의 터미널을 켜서 실행 (Send)
```bash
./audiocapture
```
또 하나의 터미널을 켜서 실행 (Receive)
```bash
ffmpeg -i rtp://127.0.0.1:5004 -f alsa default
```

</br>

## RaspberryPi 5 Setting

- 사운드카드
- 3.5mm 핀 마이크
- 스피커
  
<img width="884" alt="image" src="https://github.com/user-attachments/assets/ed5c69e9-2c98-4297-ba8f-4182d21f154d">

</br>

## 결과

<img width="884" alt="rtcp" src="https://github.com/user-attachments/assets/4aef5f9c-be53-414c-a2d8-fa6eac25c4cc">


