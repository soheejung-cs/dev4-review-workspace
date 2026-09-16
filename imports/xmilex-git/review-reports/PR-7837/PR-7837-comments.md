# PR #7837 리뷰 코멘트

1. `src/broker/cas_dispatch.c:529`

`REQUEST_BODY_MAX`가 선언만 되고 사용되지 않아 driver가 보낸 signed body length가 그대로 `MALLOC`에 들어갑니다. CAS가 아니라 `cub_server` 주소 공간을 할당하는 경로이므로 음수와 16 MiB 초과를 읽기 전에 거절해야 하는 것으로 보입니다.

2. `src/connection/adoption.cpp:1034`

`wait_for`가 timeout이어도 release 빌드에서는 `assert`가 사라져 `adoption.cpp:1061`에서 manager를 해제합니다. `registry`가 남아 있으면 종료를 계속하지 않거나 세션 스레드를 모두 join한 뒤 해제해야 하는 것으로 보입니다.

3. `src/broker/cas_server_support.cpp:183`

세션마다 `cas_Shm_stub`의 plain field를 쓰는 동안 `cas_log.c:391`, `cas_function.c:478`에서 동시에 읽고 있습니다. 같은 값을 쓰더라도 C++ data race이므로 이 값들은 세션별 TLS snapshot으로 옮기는 것이 좋겠습니다.

4. `src/executables/csql.c:3183`

SIGINT handler에서 호출하는 `csql_wire_cancel()`이 `socket`, `connect`, `getaddrinfo`, blocking I/O를 수행하고 있습니다. handler에서는 flag만 세우고 cancel 전송은 별도 안전한 실행 문맥으로 넘기는 것이 좋겠습니다.

5. `src/connection/adoption.cpp:878`

HELLO/admission 전에 연결마다 `std::make_shared`와 `std::thread`를 만들고 `adoption.cpp:902`의 예외를 잡지 않습니다. stalled connection이 누적되어 thread 생성이 실패하면 `std::terminate`가 되므로 연결 수를 제한하고 실패를 접속 거절로 처리해야 하는 것으로 보입니다.

6. `src/broker/broker_direct.cpp:1`

신규 C/C++ 파일 18개의 header가 프로젝트 표준 license template과 일치하지 않아 `license` check가 실패합니다. CI에 나온 파일들을 표준 header로 맞춰야 하는 것으로 보입니다.

7. `src/connection/server_support.c:712`

adoption endpoint 시작에 실패해도 debug log만 남기고 서버 기동을 성공 처리합니다. broker handoff가 유일한 driver 경로라면 살아 있지만 접속할 수 없는 서버가 되므로 기동을 실패시키거나 명시적인 fallback을 제공해야 하는 것으로 보입니다.

8. `src/broker/broker_direct.cpp:697`

NIT: `cas_common_main.c:225-233` 같은 파일+라인 참조가 신규 주석 9곳에 있습니다. 코드 이동 후에도 유효하도록 대상 심볼 이름으로 바꾸는 것이 좋겠습니다.

9. `src/broker/broker_direct.cpp:19`

NIT: 75개 파일의 신규 주석에 `stage B1`, `wf122/B5`, `codex F*` 같은 작업 표식이 남아 있습니다. 코드만 읽어도 유지되는 설계 이유로 정리하는 것이 좋겠습니다.

10. `src/broker/broker_direct.cpp:36`

NIT: 신규 include 묶음 10곳이 `config.h` -> system header -> CUBRID header 순서를 지키지 않습니다. `adoption.cpp`, `driver_session.cpp`, `server_compile_tracer.cpp`도 함께 정리하는 것이 좋겠습니다.

11. `src/executables/csql_wire.c:136`

NIT: wire bit shift, epoll batch/timeout, listener backlog, shutdown timeout이 숫자 리터럴로 남아 있습니다. 프로토콜과 운영 의미가 드러나는 상수로 명명하는 것이 좋겠습니다.
