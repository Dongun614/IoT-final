# app_model.py

"""
AppModel for Raspbot Multi Controller

- bot_count: 제어할 봇 수 (기본 4)
- server_ips: 각 봇의 IP 주소 리스트
- is_connected: 각 봇의 연결 상태 리스트 (bool)
- dead_reckoner: DeadReckoner 인스턴스 (외부 모듈이 없을 경우 간단한 fallback 제공)
"""

class AppModel:
    def __init__(self, bot_count=4):
        self.bot_count = bot_count

        # 각 봇의 기본 IP (사용자가 입력하면 덮어써짐)
        self.server_ips = ["192.168.0.10"] * bot_count

        # 각 봇의 연결 상태 (리스트로 관리)
        self.is_connected = [False] * bot_count

        # Dead Reckoner: 외부 모듈이 있으면 import, 없으면 간단한 내부 구현 사용
        try:
            # 프로젝트에 dead_reckoner.py가 있고, 그 안에 DeadReckoner 클래스가 있어야 함
            from dead_reckoner import DeadReckoner  # noqa: E402
            self.dead_reckoner = DeadReckoner()
        except Exception as e:
            # import 실패 시 fallback 클래스 사용
            # (개발/테스트 시에는 이 간단한 버전으로도 UI 동작 확인 가능)
            print(f"[AppModel] Warning: Could not import DeadReckoner ({e}). Using fallback DeadReckoner.")

            class _FallbackDR:
                def __init__(self):
                    # 위치/자세 기본값
                    self.X_POS = 0.0
                    self.Y_POS = 0.0
                    self.THETA = 0.0  # radians

                def reset(self):
                    self.X_POS = 0.0
                    self.Y_POS = 0.0
                    self.THETA = 0.0

                def integrate_motion(self, vx, vy, omega, dt):
                    """
                    단순 통합 (테스트 목적)
                    vx, vy : m/s (로컬 프레임 가정)
                    omega  : rad/s
                    dt     : seconds
                    """
                    # 아주 단순한 직선 통적분 (로컬->월드 변환 생략: 테스트 용)
                    self.X_POS += vx * dt
                    self.Y_POS += vy * dt
                    self.THETA += omega * dt

            self.dead_reckoner = _FallbackDR()
