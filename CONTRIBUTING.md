# 기여 규약 (dev4-review-workspace · dev4-ai-source 공통)

1. **이름을 남긴다.** 커밋 저자는 사람 이름(`git config user.name "<한글 이름>"`, 리포별 설정), 기록 항목·파일에는
   `이름(.컨테이너) 날짜` 서명. Claude 세션은 `Co-Authored-By:` 트레일러로만. 설정 절차: `members/_설정-템플릿.md`.
2. **한 발견은 한 곳에.** 어디에 둘지는 `skills/record/SKILL.md` 판단표. 소스 사실은 `dev4-ai-source/staging/<이름>-<이슈명>.md` 로.
3. **컨테이너·머신 고유 사실은 `members/<이름>/`** 에만. 공용 스킬·참고 문서에는 넣지 않는다.
4. **규약을 바꾸면 그 줄에 이름과 지시 날짜.** 스킬(`skills/*/SKILL.md`)이 곧 규약이다 — 채팅에만 남긴 지시는 없는 것과 같다.
5. push 전에 `git pull --rebase`. 세 컨테이너·여러 사람이 같은 리포에 push 한다.
6. 프라이빗 리포다 — 초대는 `members/README.md` 명단 기준으로 owner(정소희)가 한다.
