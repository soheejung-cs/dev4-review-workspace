# shell TC 분류·인덱스 — 산출물은 커밋하지 않는다

`index.json` / `index.md` / `by-tag.md` 는 **cubrid-testcases-private-ex(비공개)** 의 케이스 경로·주석·이슈 키를 담으므로 공개 리포에 올리지 않는다(`.gitignore`).
로컬에서 생성: `python3 tools/tc_index_build.py` (기본 `~/dev/sources/cubrid-testcases-private-ex` → 여기). 판정: `python3 tools/tc_relevance.py --pr <n> --failed <케이스…>`.
