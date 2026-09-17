---
name: review-recommend
description: PR 리뷰어 추천 — `/review-recommend <PR>`. 최근 머지된 PR(작성·리뷰) 인덱스로 관심도, 변경 규모·영역으로 난이도(간단 1명 / 보통 2명 / 어려움 3명), review-to-do 보드로 부하(대기·진행 중)를 합쳐 누가 리뷰하면 좋을지 표로 낸다. "누가 리뷰하면 좋을까", "리뷰어 추천", "리뷰어 배정 균형".
---

# /review-recommend <PR>

인자: PR 번호(기본 저장소 `CUBRID/cubrid`; 다른 저장소는 `--repo`). 추천만 한다 — **reviewer 지정은 사람이** (`gh pr edit --add-reviewer`, 출력 마지막 줄에 명령이 있다).

```bash
cd ~/dev/docs/dev4-review-workspace && python3 tools/review_recommend.py <PR> [--refresh] [--json] [--pool a,b]
```

첫 실행은 머지 PR 인덱스(18개월, 약 1,600건, GraphQL 50건/페이지 ≈ 50회)를 만들어 `~/dev/utils/review-recommend/merged_index.json` 에 저장한다 — 1~2분. **한 번만 수집하고 자동 갱신하지 않는다**(사용자 지시 2026-09-17 — 토큰·호출 절약). PR 마다 작성자·리뷰어·**모듈(디렉터리 2단계)** 만 남기고 파일 목록은 버린다. 다시 받고 싶을 때만 `--refresh`.

## 세 축

| 축 | 어디서 | 계산 |
|---|---|---|
| **관심도** | 머지 PR 인덱스(작성자·리뷰어·모듈) | 머지 PR 마다 `sim = (겹친 모듈 수) / (대상 PR 모듈 수)`, 역할 가중(작성 1.0 / 리뷰 0.6), 최근성 `0.5^(개월/12)`. 후보별 합. 모듈 = `src/optimizer` 같은 2단계 디렉터리 |
| **난이도** | 대상 PR | 점수 = 변경 줄(<50:0, <200:1, <600:2, ≥600:3) + 파일 수(<3:0, <8:1, ≥8:2) + 디렉터리 수(1:0, 2:1, ≥3:2) + 어려운 영역(storage·transaction·query·optimizer·thread·connection·communication·replication·parser, 또는 btree·heap·log_·lock_·mvcc·pgbuf·xasl·scan·vacuum 파일) 2 + 제목 키워드(refactor·parallel·lock·mvcc·deadlock·crash… +1, typo·backport·doc·comment·message… −1). **≤2 간단→1명, ≤5 보통→2명, 그 위 어려움→3명** |
| **부하** | review-to-do 보드 `board.json`(10분 주기, 90분 넘게 낡았으면 GitHub 직접) | 후보가 **요청됐지만 아직 리뷰 안 한** PR 1.0 + **리뷰했지만 승인 전이고 PR 이 열린** 것 0.5. 자기 PR 은 제외(따로 표시) |

**종합** = 0.6·관심도(최대값으로 정규화) + 0.4·(1 − 부하 정규화). 부하가 **평균 + 2 이상**인 후보는 다른 후보로 인원을 채울 수 있으면 뒤로 뺀다(공평성). 작성자는 후보에서 제외, 이미 요청·리뷰한 사람은 "현재" 열에 표시.

**학습 슬롯** (사용자 지시 2026-09-17): 리뷰어가 **2명 이상**이면 `learner_prob`(기본 0.5) 확률로 마지막 한 자리를 **그 모듈 이해가 낮은 사람**에게 준다 — 성능 개선팀은 관심 없는 파트도 공부해야 하므로. 후보 중 관심도 하위 절반에서 부하가 가장 낮은 사람. 난수 시드는 PR 번호라 **같은 PR 은 항상 같은 답**(재실행해도 바뀌지 않음). 출력에 `(학습 슬롯)` 으로 표시된다.

후보 = `tools/roster.json` 의 `reviewer_pool`(있으면) 또는 `tracked_github`. 가중치·영역 목록은 `roster.json` 의 `recommend` 객체로 덮어쓸 수 있다(키는 `review_recommend.py` 의 `DEFAULT_CFG`).

## 절차
1. 도구를 돌려 표를 받는다. 인덱스 구축 로그는 stderr.
2. 사용자에게 **추천 + 근거(각 후보의 대표 머지 PR·부하)** 를 그대로 보이고, 난이도 판정 이유 줄도 함께. 판정이 틀려 보이면(예: 한 줄 수정인데 어려움) 이유 줄에서 어느 항이 올렸는지 보인다.
3. 지정은 하지 않는다. 사용자가 원하면 마지막 줄의 `gh pr edit` 명령을 보여 준다(실행도 사용자 확인 후).

## 한계 (정직하게)
- 파일 100개 초과 PR 은 앞 100개만 본다(GraphQL `files(first:100)`).
- 관심도는 **머지된** PR 만, 그리고 **인덱스를 만든 시점까지만** 본다 — 자동 갱신이 없으므로 몇 달 지나면 `--refresh` 를 한 번 돌린다(인덱스 생성일은 출력 마지막 줄에 있다). 열린 PR 에서의 활동은 보드(부하)에만 반영된다.
- 난이도는 규모·영역 휴리스틱이다. 설계 난이도(불변조건·락)는 제목·영역으로만 근사한다.
- 후보 풀 밖의 사람(예: 보드 미추적 리뷰어)은 추천되지 않는다 — 풀에 넣으려면 `roster.json`.
