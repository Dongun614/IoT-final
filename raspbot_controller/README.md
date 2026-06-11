# Raspbot Multi-Controller

최대 4대의 라즈베리파이 기반 로봇(Raspbot)을 동시에 제어하기 위한 GUI 애플리케이션

PC에서 실행되는 컨트롤러 애플리케이션과 각 로봇에서 실행되는 서버 애플리케이션으로 구성, TCP/IP 소켓을 통해 통신함

## 프로젝트 구조

```
/
├── app.py                  # PC용 메인 컨트롤러 GUI 애플리케이션 (실행 파일)
├── app_model.py            # GUI 앱의 데이터 및 상태 관리 (모델)
├── app_controller.py       # GUI 앱의 비즈니스 로직 (컨트롤러)
├── socket_controller.py    # PC-로봇 간의 소켓 통신 관리
│
├── raspbot_server.py       # 각 라즈베리파이 로봇에서 실행될 서버 프로그램
└── YB_Pcb_Car.py           # 라즈베리파이 로봇의 모터/서보 제어를 위한 I2C 라이브러리
```

## 파일 상세 설명

### 1. PC 컨트롤러 애플리케이션

-   **`app.py`**:
    -   Tkinter를 사용하여 제작된 메인 GUI 애플리케이션
    -   최대 4개의 로봇에 대한 제어판을 제공
    -   사용자는 각 로봇의 IP 주소를 입력하고, 연결/해제 가능
    -   PWM 값과 작동 시간을 설정하여 로봇을 지정된 시간 동안 전진
    -   MVC(Model-View-Controller)와 유사한 구조로 `app_model.py`, `app_controller.py`와 상호작용

-   **`app_model.py`**:
    -   애플리케이션의 데이터 관리(Model)
    -   제어할 로봇의 수, 각 로봇의 IP 주소, 연결 상태 등의 정보를 저장

-   **`app_controller.py`**:
    -   애플리케이션의 핵심 로직을 담당(Controller)
    -   GUI(`app.py`)의 사용자 입력(버튼 클릭 등)을 받아 `socket_controller.py`에 명령을 전달하는 역할
    -   예: "Timed Forward" 버튼 클릭 시, 모터 작동 명령을 보낸 후 지정된 시간이 지나면 정지 명령을 보내는 로직처리

-   **`socket_controller.py`**:
    -   TCP 클라이언트 역할을 수행하며, 각 라즈베리파이 로봇 서버에 대한 소켓 연결을 생성하고 관리
    -   `app_controller.py`로부터 받은 명령을 로봇이 이해할 수 있는 형식의 패킷으로 변환하여 전송(예: `M,150,150`)

### 2. 라즈베리파이 로봇 소프트웨어

-   **`raspbot_server.py`**:
    -   각 라즈베리파이 로봇에서 실행되어야 하는 TCP 서버 프로그램
    -   `0.0.0.0:12345` 주소에서 PC 컨트롤러의 연결을 기다림
    -   컨트롤러로부터 수신한 패킷(예: `M,150,150`)을 분석하여 `YB_Pcb_Car.py` 라이브러리를 통해 실제 하드웨어를 제어
    -   수신된 명령에 대한 응답(예: `OK,M,150,150`)을 PC로 전송

-   **`YB_Pcb_Car.py`**:
    -   Yahboom 사의 로봇 하드웨어 제어용 Python 라이브러리
    -   I2C 통신을 통해 모터 드라이버 보드에 신호를 보내 좌/우 모터의 방향과 속도(PWM), 서보 모터의 각도 등을 제어

## 실행 방법

### 1. 라즈베리파이 로봇 설정

1.  각 라즈베리파이 로봇에 `raspbot_server.py`와 `YB_Pcb_Car.py` 파일을 복사
2.  로봇의 터미널에서 다음 명령어를 실행하여 서버 시작
    ```bash
    python3 raspbot_server.py
    ```
3.  서버가 실행되면 `Raspbot Server Running on 12345` 메시지가 표시
4.  `ifconfig` 또는 `ip a` 명령어로 로봇의 IP 주소를 확인

### 2. PC 컨트롤러 실행

1.  PC에 `app.py`, `app_model.py`, `app_controller.py`, `socket_controller.py` 파일이 모두 있는지 확인
2.  터미널에서 다음 명령어를 실행하여 GUI 컨트롤러를 시작
    ```bash
    python3 app.py
    ```
3.  GUI가 나타나면, 제어할 로봇의 IP 주소를 "IP" 필드에 입력하고 "Connect" 버튼을 클릭
4.  연결에 성공하면 버튼이 녹색 "Disconnect"로 바뀜
5.  "PWM"과 "Duration" 값을 설정한 후 "START" 버튼을 누르면 로봇이 지정된 시간 동안 전진 후 정지
6.  "STOP" 버튼은 언제든지 로봇을 즉시 정지

## 통신 프로토콜

PC와 로봇 간의 통신은 간단한 텍스트 기반 프로토콜을 사용

-   **모터 제어**: `M,left_pwm,right_pwm`
    -   예: `M,150,150` (양쪽 모터를 150 PWM으로 전진)
-   **정지**: `S`
-   **서보 제어**: `SERVO,id,angle`
    -   예: `SERVO,1,90` (1번 서보를 90도로 이동)
