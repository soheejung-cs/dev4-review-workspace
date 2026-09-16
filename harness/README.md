# dev4 하네스 — 설계용 / 리뷰용 (레퍼런스 공유), TC 는 별도 리포

```
dev4-review-workspace/
  harness/
    pipelines/review.yaml     리뷰용 실행 그래프 (initialize → context → review(LLM) → adjudicate → publish)
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

## 실행
```
cd ~/dev/docs/dev4-review-workspace
python3 -m tools.harness.run review --pr 7937            # out/<pr>/<sha>/{codegraph.sqlite3, context_pack.md, manifest.json}
python3 -m tools.harness.run design --pr 7899            # + arch.json, arch.mmd, arch.excalidraw
python3 -m tools.harness.run adjudicate --pr 7937 --findings findings.json   # 스키마 검증→판정→requery.json→episodic 적재
# 산출물: manifest.json(결정론 기록), context_pack[.batchN].md, findings.auto.json(MEAS), findings.adjudicated.json, requery.json
```
전제: `~/dev/utils/tree-sitter/langs.so` (tree-sitter-c v0.20.6 + tree-sitter-cpp v0.20.3, `Language.build_library`), `pip3 install --user "tree_sitter<0.21"`. 없으면 ctags 폴백(호출 그래프 없음, `codegraph_complete=false`).

## LLM 이 하는 일 / 하지 않는 일
- 한다: `context_pack.md`(+ `arch.json`)만 읽고 `finding.json` 스키마로 지적을 낸다. 규칙은 ID 로 인용한다.
- 하지 않는다: 저장소 전체 검색, 줄 번호 추측, 판정. 판정은 `adjudicate` 가 결정론적으로 한다(anchor·evidence·rule·graph 의무).
