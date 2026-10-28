"""규칙 ID 꼴 — 하네스가 `rules/*.md` 판정 행과 finding 의 rule_ids 를 읽는 한 곳(H-28).

예전에는 context_pack(`[A-Z]{2,5}-\\d+`)·finding.json(`^[A-Z]{2,5}-\\d{2}$`)·adjudicate(`[A-Z]{2,5}-\\d{2}`) 가 "대문자-숫자 2자리" 만
받아, 규칙 문서에 실제로 있는 꼴 `MEAS-R-26`(§6)·`APX-A-01`(부록)·`A01`(17장 안티패턴)·`PRIO-04.5`(§5)·`INV-1`(설계 §2) 을 못 읽었다 —
행이 팩에 주입되지 않고, 그 ID 를 인용한 finding 은 스키마 실패로 requery 로 갔다. 꼴을 여기 하나에 두고 셋이 같이 쓴다.
`harness/schemas/finding.json`·`plan.json` 의 rule_ids pattern 은 JSON 이라 import 할 수 없다 — `ID_PATTERN` 과 같은 문자열을 두고
tests/test_rule_ids.py 가 둘이 같은지 본다.

  ID 꼴     접두(대문자 2~6) [-하위(대문자 1~2)] -번호(1~3자리) [.소수]   예) MEM-03, MEAS-R-26, APX-A-01, PRIO-04.5, INV-1
            또는 대문자 1개 + 번호 2~3자리                                 예) A01 (17장 안티패턴 표)
  축(axis)  번호 앞 부분: MEAS-R-26 → MEAS-R, APX-A-01 → APX-A, A01 → A, PRIO-04.5 → PRIO
  판정 행   `| <ID 칸> | …` 로 시작하는 표 행. ID 칸이 `BR-04/A59` 처럼 둘이면 둘 다 그 행의 ID 다.
"""
import re
from typing import Iterable, List, Optional, Set

ID_PATTERN = r'^(?:[A-Z]{2,6}(?:-[A-Z]{1,2})?-\d{1,3}(?:\.\d+)?|[A-Z]\d{2,3})$'
ID_RE = re.compile(ID_PATTERN)
# 문장 속 인용을 찾는 꼴. 한 글자 꼴(A01)은 줄 번호 표기(L26)와 겹치므로 여기서는 찾지 않는다 — 그 꼴은 판정 행의 ID 칸으로만 안다
_MENTION_RE = re.compile(r'\b([A-Z]{2,6}(?:-[A-Z]{1,2})?-\d{1,3}(?:\.\d+)?)(?![\d.]*\d)')
# 예전 판정기가 문장 속에서 알던 꼴 — 그 집합은 그대로 '아는 ID' 로 남긴다(전에 통과하던 finding 이 새로 거절되지 않게)
_LEGACY_MENTION_RE = re.compile(r'\b([A-Z]{2,5}-\d{2})\b')
_SPLIT_RE = re.compile(r'[/,\s]+')


def is_rule_id(s: str) -> bool:
    return bool(ID_RE.match(s or ''))


def axis(rule_id: str) -> str:
    """번호 앞 부분 — 축 표의 이름. MEAS-R-26 → 'MEAS-R', A01 → 'A'."""
    m = re.match(r'^([A-Z]+(?:-[A-Z]{1,2})?)-?\d', rule_id)
    return m.group(1) if m else rule_id


def row_ids(line: str) -> List[str]:
    """판정 행이면 ID 칸의 ID 목록(`BR-04/A59` → ['BR-04', 'A59']), 아니면 []. 머리 행(`| ID |`)·구분 행은 []."""
    if not line.startswith('|'):
        return []
    cell = line.split('|', 2)[1].strip()
    parts = [p for p in _SPLIT_RE.split(cell) if p]
    if not parts or not all(is_rule_id(p) for p in parts):
        return []
    return parts


def violated_axes(line: str) -> Set[str]:
    """행 문장이 'X 위반'·'위반 X' 로 가리키는 규칙의 축 — 17장 안티패턴(A01 → ALLOC-01 위반) 행이 그 축과 함께 주입되게."""
    out = set()
    for m in re.finditer(r'위반\s+([A-Z][A-Z0-9-]*\d)|([A-Z][A-Z0-9-]*\d)\s+위반', line):
        rid = m.group(1) or m.group(2)
        if is_rule_id(rid):
            out.add(axis(rid))
    return out


def known_ids(rules_text: str) -> Set[str]:
    """판정기가 아는 규칙 ID: rules/*.md 판정 행의 ID 전부(모든 꼴) + 문장 속 인용(예전 꼴과 새 꼴)."""
    ids = set(_LEGACY_MENTION_RE.findall(rules_text)) | set(_MENTION_RE.findall(rules_text))
    for line in rules_text.split('\n'):
        ids.update(row_ids(line))
    return ids


def normalize(rule_ids: Optional[Iterable[str]]) -> List[str]:
    """finding 의 rule_ids 정리 — `BR-04/A59` 처럼 판정 행의 ID 칸을 그대로 옮긴 것은 나누고, 공백을 걷고, 순서를 지켜 중복을 뺀다."""
    out = []
    for r in rule_ids or []:
        for p in _SPLIT_RE.split(str(r).strip()):
            if p and p not in out:
                out.append(p)
    return out
