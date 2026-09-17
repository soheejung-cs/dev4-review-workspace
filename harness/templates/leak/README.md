# 누수 측정 템플릿 (2026-09-17, CBRD-27215 에서 뽑음)

답안 비교가 못 잡는 것을 재는 스크립트 셋. 원본은 `claude-workspace/projects/CBRD-27215/repro/`.
전부 `INSTALL=<설치본>` 으로 대조군을 지정할 수 있다 — **develop 설치본을 같은 방법으로 함께 재는 것이 판정의 전제다.**

| 스크립트 | 무엇 | 판정 |
|---|---|---|
| `noclone_leak2.sh` | 한 서버에서 같은 배치 3회, RSS 비교. `CLONES=`/`ENTRIES=` 로 플랜 캐시 모양 지정 | 2·3회차 증가분이 0. 1회차는 워밍업이라 버린다 |
| `memmon_check.sh` | `enable_memory_monitoring=yes` + `cubrid memmon -o` 를 배치 전후로. 파일:줄 단위 | 내가 만진 파일의 줄이 배치마다 늘지 않는다 |
| `valgrind_leak.sh` | SA 모드(`csql -S`) 를 memcheck 로. 서버가 한 프로세스라 캐시 retire 까지 잡힌다 | `definitely lost` 0 |

## 쓰기 전에 고칠 것
- 스크립트 안의 `I=${INSTALL:-...}` 기본값, `DB=`, 그리고 워크로드 SQL(테이블·질의) 은 대상 변경에 맞게 바꾼다.
- `set_conf` 가 손대는 설치본은 **테스트 전용**이어야 한다. `~/CUBRID` 가 가리키는 활성 설치본에서 돌리지 않는다.
- `ninja install` 이 `conf/cubrid.conf` 를 템플릿으로 되돌린다. **빌드·설치가 끝난 뒤에** conf 를 세우고 측정한다(설치 중에 재면 설정이 날아간 채로 측정된다 — 실제로 한 번 당했다).

## 함정
- 빌드 완료 판정을 로그의 마지막 빌드 줄로 하면 안 된다. `ninja install` 이 아직 돌고 있다. 동기 실행하거나 `ninja` 프로세스가 사라질 때까지 기다린다.
- 설치본 라이브러리는 빌드 트리와 **해시가 다르다**(RPATH 를 install 시점에 덮어쓴다). 최신 여부는 해시가 아니라 크기·시각으로 본다.
