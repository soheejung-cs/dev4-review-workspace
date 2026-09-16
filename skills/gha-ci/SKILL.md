---
name: gha-ci
description: gha-ci(GitHub Actions self-hosted 샤드) 실패를 분석한다 — PR 헤드 SHA 의 런을 찾아 collect 요약·failed.list·샤드 test-shell.xml 을 읽고, 코어덤프는 아티팩트 서버(192.168.1.48:30080)의 코어 + 같은 SHA 의 debug 빌드로 assert 지점을 파일:줄까지 확정한다. "CI 실패 봐줘", "gha-ci NOK", "코어 떴어" 때. (구 CircleCI 산출물 다운로드 방식은 2026-09-16 폐기)
---

# gha-ci 분석 (gha-ci)

CircleCI 시절의 "API 로 아티팩트 다운로드" 는 더 쓸 수 없다(2026-09-16). gha-ci 는 결과를 GitHub artifact 로 올리지 않고
**self-hosted 저장소**에 두고, 그 저장소를 내부망 http 로 노출한다. 이 컨테이너(.51)에서 열린다.

## 0. 어디에 무엇이 있나
| 것 | 위치 |
|---|---|
| 런 찾기 | `gh run list --repo CUBRID/cubrid --workflow gha-ci.yml --branch <브랜치> --json databaseId,headSha,conclusion,createdAt` — **PR 헤드 SHA 와 headSha 가 같은 런**만 그 PR 의 결과다 |
| 잡 | `gate` → `build (release|debug)` → `plan` → `shard NN`(50개, 각각 CTP shell 을 분담) → `collect` → `status`. `/run all` 코멘트 없으면 shard 이후는 skipped |
| 실패 요약 | `collect` 잡의 Step Summary — 실패 케이스 표 + 케이스별 `<failure>` 로그(잘림). 로그로도 남는다: `gh run view <run> --repo CUBRID/cubrid --job <collect job id> --log` 에서 `=== summary ===` 이후 |
| 실패 목록 파일 | `http://192.168.1.48:30080/runs/<run_id>/shell/failed.list` (`<shard>\t<케이스 경로>`) |
| 샤드별 결과 | `http://192.168.1.48:30080/runs/<run_id>/shell/results/<shard>/` — `test-shell.xml`(CTP JUnit, 케이스별 `<failure>` 전문) · `test_status.data` · `feedback.log` · `tc.list` · **`coredump/`** |
| 코어덤프 | 같은 디렉터리 `coredump/cub_server_*.coredump …`. **1 GiB 초과 코어는 복사되지 않고 `coredump/oversized.tsv` 에 이름·크기만** 남는다 |
| 그 런이 테스트한 빌드 | `http://192.168.1.48:30080/builds/{develop|pr}/<sha40>/{debug,release}/CUBRID/` + `build.meta`(sha/mode/run_id). **코어 해석에는 `debug/CUBRID/bin/cub_server` 등 이 빌드를 쓴다** — 로컬 빌드와 주소가 다르다 |
| 판정 상태 | `status` 잡이 헤드 커밋에 `gha-ci: test_shell` 컨텍스트로 success/failure 를 단다 |

## 1. 순서
1. PR 헤드 SHA → 런: `gh pr view <n> --repo CUBRID/cubrid --json headRefOid` 와 `gh run list ... --json headSha` 대조. 헤드가 바뀌었으면 옛 런은 옛 코드의 결과다.
2. `collect` 로그의 `run/passed/failed/skipped` 와 실패 표를 읽는다. **`Check TC PRs` 는 무시**(PR브랜치-규칙 §5). 샤드가 다른 빌드를 읽었다는 `::error::shards read different builds` 가 있으면 결과 전체가 무효.
3. 실패 케이스마다 `results/<shard>/test-shell.xml` 의 `<failure>` 전문을 받아 3분류(출력차 / 코어 / 미상)한다 — 분류 규약은 `tc-analysis` 스킬. 출력차는 답안 정렬 대상, 코어는 아래로.
4. 코어: `results/<shard>/coredump/` 목록 → 파일 다운로드(`curl -O`) → 같은 런의 `build.meta` 로 SHA·mode 확정 → `builds/.../debug/CUBRID/` 에서 바이너리 받아 §2~§3 방법으로 해석. `oversized.tsv` 에만 있으면 "코어 있음·미해석(1GiB 초과)" 으로 보고하고 로컬 debug 재현으로 넘어간다.
5. 결과는 `record` 스킬대로 `claude-workspace/projects/<JIRA키>/` 에 남기고, 재현되면 `dev4-ai-source/staging/<이름>-<JIRA키>.md` 에 코드 사실을.

주의: `_fork/`, `_probe/` 디렉터리는 CI 자체 점검용이다. 답안 분석 때 **코드베이스를 읽지 않는** 제약은 `tc-analysis` 와 같다 — 코어 해석만 예외적으로 바이너리·심볼을 본다.

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
