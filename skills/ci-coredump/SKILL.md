---
name: ci-coredump
description: CI 실패 잡의 코어덤프를 그 잡의 build_debug 산출물로 풀어 assert 지점을 파일:줄까지 확정한다. 코어 NOK 를 분석할 때(CircleCI API 토큰 불필요).
---

CI 에서만 나고 로컬에서 재현이 안 되는 크래시·assert 의 **정확한 발화 지점**을 찾는 경로다.
2026-09-01 CBRD-27355 에서 실제로 이걸로 확정했다(스택의 static 3프레임 → `btree.c:30574`).

## 1. 산출물은 토큰 없이 받을 수 있다

CUBRID 가 공개 리포라 CircleCI API 가 인증 없이 열린다.

```bash
J=150402                                   # 실패한 잡 번호 (PR 체크 링크에 있다)
curl -s "https://circleci.com/api/v1.1/project/github/CUBRID/cubrid/$J/artifacts"
curl -s "https://circleci.com/api/v1.1/project/github/CUBRID/cubrid/$J" | python3 -c "
import json,sys; d=json.load(sys.stdin); print(d['workflows']['workflow_id'])"
curl -s "https://circleci.com/api/v2/workflow/<workflow_id>/job"   # 같은 워크플로의 다른 잡
```

필요한 것 두 개:

| 무엇 | 어디 |
|---|---|
| **코어덤프 텍스트** | 실패 잡(test_sql/test_shell)의 `tmp/logs/cubrid_log/coredump/cub_server_*.coredump` |
| **그 실패 빌드 그대로의 바이너리** | 같은 워크플로 `build_debug` 잡의 `CUBRID.tar.gz` (약 290M) |

`build_debug` 잡 번호는 위 v2 workflow API 로 찾는다. 다운로드가 느리므로(수 분) 백그라운드로.
서버 err 로그에는 assert 메시지가 없다(stderr 미수집) — 코어덤프 텍스트가 유일한 스택이다.

## 2. 코어덤프는 절대주소 + 최근접 심볼이다

`er_dump_call_stack` 이 dladdr 로 찍으므로 **static 함수 프레임은 "unknown function"** 으로 나오고,
이름이 붙은 프레임도 최근접 심볼이라 정확하지 않다. 쓸 수 있는 것은 **절대주소**뿐이다.

```
[18] 0x0000762537d3f2b7: unknown function at /home/CUBRID/lib/libcubrid.so.11.5
[15] 0x0000762537d302f5: _Z12btree_insert... at /home/CUBRID/lib/libcubrid.so.11.5
```

또 `__assert_perror_fail` 로 찍혀도 실제로는 `__assert_fail`(평범한 `assert()`)일 수 있다 —
libc 안 인접 심볼로 잡힌 것이다. 소스에 `assert_perror` 사용처가 있는지 grep 해 보면 구분된다.

## 3. ⚠ 로드 베이스를 심볼 주소로 잡으면 틀린다

스택의 값은 **호출 복귀 주소**(symbol + delta)라, `base = 스택주소 - 심볼오프셋` 은 delta 만큼 어긋난다.
어긋난 베이스로 `addr2line` 하면 **그럴듯하지만 틀린 줄**이 나온다(실제로 `pgbuf_unfix_and_init` 줄이
나왔다가 재계산하니 다른 함수였다).

**맞추는 방법**: 프레임 간 거리는 베이스와 무관하다. 두 프레임의 성격을 이용해 교차시킨다.

```bash
L=CUBRID/lib/libcubrid.so.11.5
nm -C $L | grep -E 'btree_split_node_and_advance|^[0-9a-f]+ [tT] btree_insert\('   # 함수 범위
objdump -d --start-address=0x<함수시작> --stop-address=0x<끝> $L > fn.asm
grep -A1 'callq.*assert_fail' fn.asm    # assert 호출의 '복귀 주소' 후보들
```

`[18]` 은 `__assert_fail` 호출의 복귀 주소여야 하고, `[15]` 는 `btree_insert_internal` 호출의 복귀
주소여야 한다. 후보들 중 **두 주소의 차이가 스택의 두 절대주소 차이와 같은 쌍**이 유일하게 맞물린다.

```python
delta = run18 - run15                       # 스택에서 읽은 절대주소 차
for a18 in assert_return_addrs:             # 함수 내 assert 복귀 주소 후보
    if (a18 - delta) in call_return_addrs_in_btree_insert:
        base = run18 - a18                  # ★ 확정
```

## 4. 확정 후

```bash
addr2line -f -C -i -e $L 0x<오프셋>          # 함수명 + 파일:줄 (인라인 포함)
objdump -d --start-address=... --stop-address=... $L | tail -20
```

디스어셈블로 **교차 검증까지 한다.** `__assert_fail (assertion, file, line, function)` 의 3번째 인자가
`mov $0x<line>,%edx` 로 실려 있어 줄번호가 코드에 그대로 박혀 있다(`0x776e` = 30574).
매크로 안의 assert 는 `__LINE__` 이 **호출 지점 줄**로 잡히므로, 소스의 그 줄이 매크로 호출이면
(`ASSERT_ERROR_AND_SET`, `pgbuf_unfix_and_init` 등) 실제 assert 는 매크로 정의 안에 있다.

## 5. 이 방법의 한계

- **변수 값은 못 본다.** 텍스트 코어덤프에는 스택 프레임만 있다. 로컬 변수가 필요하면 진짜 코어 파일이
  있어야 한다(gdb). CBRD-27354 처럼 이슈 본문에 gdb 로컬 변수가 인용돼 있으면 그건 별도 수집분이다.
- 따라서 **"어느 assert 가 깨졌나"는 확정되지만 "왜 그 조건이 됐나"는 코드 경로 추론으로 메워야 한다.**
  그 추론이 유일해지도록, 도달 가능한 다른 실패 경로가 각자 다른 프레임에서 죽는다는 것까지 확인한다.

관련: [[빌드환경-규칙]](빌드·리비전 고정), [[작업-생애주기]] §1-(B)(분석 작업 절차)
