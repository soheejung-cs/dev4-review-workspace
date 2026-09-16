# dev4 하네스 — 설계용 / 리뷰용 (레퍼런스 공유), TC 는 별도 리포

> 처음이면 리포 루트 `팀-안내.md`(사용법 5분 + 만든 과정) 부터.

```
dev4-review-workspace/
  harness/
    pipelines/full.yaml       **통합** 실행 그래프 — 리뷰(성능 규칙)+설계(불변조건·계층·리스크)+판정·메모리를 한 명령으로
    pipelines/review.yaml     (부분집합) 리뷰용 실행 그래프 (initialize → context → review(LLM) → adjudicate → publish)
    pipelines/design.yaml     설계용 실행 그래프 (+ infer: 아키텍처 유추·리스크·다이어그램)
    pipelines/adjudicate.yaml LLM 산출물 판정 + 자기 보정(validate→gate→self_check→episodic)
    templates/repro.sh        재현 스크립트 템플릿(trap 정리·격리 포트)
    schemas/finding.json      LLM 산출물 계약 — 이 스키마를 지키지 않는 finding 은 adjudicate 가 받지 않는다
    ARCHITECTURE.md           융합 아키텍처·코드 패턴 (Metis CodeGraph/Reachability + Azure infer/risk/diagram)
    META-REVIEW.md            4 Primitives 채점과 리팩토링 포인트
  tools/harness/              구현: codegraph.py · reachability.py · context_pack.py · arch_infer.py · adjudicate.py · run.py
  rules/  references/  skills/  examples/    ← 두 하네스가 공유
dev4-tc-workspace/            TC 변경·CI 실패 분석(tc-analysis, gha-ci, shell TC 인덱스) — 리뷰 하네스와 분리
```

## 슬래시 명령
Claude Code 에서 **`/harness-review <PR번호>`** — `skills/harness-review/SKILL.md` 가 아래 3단계를 이 세션(LLM)이 수행하게 한다. 컨테이너에 `ln -sfn ~/dev/docs/dev4-review-workspace/skills/harness-review ~/.claude/skills/harness-review` 한 번.

## 실행 — 통합(권장)
```
cd ~/dev/docs/dev4-review-workspace
python3 -m tools.harness.run full --pr 7937                        # 1) 코드+설계+성능 입력 한 번에: review_request[.batchN].md, arch.json, findings.auto.json
#   2) LLM(이 에이전트)이 review_request*.md 를 읽고 out/<pr>/<sha>/findings.json 을 쓴다 (스키마 harness/schemas/finding.json)
python3 -m tools.harness.run full --pr 7937 --findings <out>/findings.json   # 3) 검증→판정→requery.json→report.md→episodic
```
부분 실행(디버그용):
```
python3 -m tools.harness.run review --pr 7937            # out/<pr>/<sha>/{codegraph.sqlite3, context_pack.md, manifest.json}
python3 -m tools.harness.run design --pr 7899            # + arch.json, arch.mmd, arch.excalidraw
python3 -m tools.harness.run adjudicate --pr 7937 --findings findings.json   # 스키마 검증→판정→requery.json→episodic 적재
# 산출물: manifest.json(결정론 기록), context_pack[.batchN].md, findings.auto.json(MEAS), findings.adjudicated.json, requery.json
```
전제: `~/dev/utils/tree-sitter/langs.so` (tree-sitter-c v0.20.6 + tree-sitter-cpp v0.20.3, `Language.build_library`), `pip3 install --user "tree_sitter<0.21"`. 없으면 ctags 폴백(호출 그래프 없음, `codegraph_complete=false`).

## Oracle 매뉴얼 원문
재배포 금지라 이 공개 리포에는 없다 → private `soheejung-cs/dev4-oracle-manual` / 내부망 http://192.168.6.51:8827/docs/dev4-oracle-manual/ (`references/oracle-manual/README.md`).

## LLM 이 하는 일 / 하지 않는 일
- 한다: `context_pack.md`(+ `arch.json`)만 읽고 `finding.json` 스키마로 지적을 낸다. 규칙은 ID 로 인용한다.
- 하지 않는다: 저장소 전체 검색, 줄 번호 추측, 판정. 판정은 `adjudicate` 가 결정론적으로 한다(anchor·evidence·rule·graph 의무).
