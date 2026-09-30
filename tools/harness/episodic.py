"""Episodic memory: 게시된 리뷰 코멘트와 작성자 응답을 examples/episodic/PR-<n>.json 으로 적재한다.
outcome: accepted(작성자가 수정/동의) | rebutted(반박) | open. 다음 리뷰의 context pack 우선순위 6 으로 들어간다."""
import json, os, re, subprocess
from typing import Dict, List

ACCEPT = re.compile(r'맞습니다|수정했습니다|반영했|동의합니다|고쳤습니다|fixed|agreed|done', re.I)
REBUT = re.compile(r'아닙니다|기존 설계|의도한|철회|not a bug|by design|오탐', re.I)

def collect(pr: int, reviewer: str, out_dir: str, repo: str = 'CUBRID/cubrid', local: List[Dict] = None) -> str:
    """local: 이 실행의 findings.adjudicated (게시 전 판정). 게시된 코멘트와 file:line(±3) 으로 짝을 지어
    posted=True/False 를 붙인다 — 리뷰 대부분이 로컬 판정에서 끝나는 지금 흐름에서 파일이 비지 않게 (2026-09-30)."""
    q = '''query($n:Int!){repository(owner:"CUBRID",name:"cubrid"){pullRequest(number:$n){author{login} reviewThreads(first:100){nodes{isResolved path line comments(first:10){nodes{author{login} body createdAt}}}}}}}'''
    d = json.loads(subprocess.run(['gh', 'api', 'graphql', '-F', f'n={pr}', '-f', f'query={q}'], stdout=subprocess.PIPE, universal_newlines=True).stdout)
    pr_node = d['data']['repository']['pullRequest']; author = pr_node['author']['login']
    items: List[Dict] = []
    for t in pr_node['reviewThreads']['nodes']:
        cs = t['comments']['nodes']
        if not cs or cs[0]['author']['login'] != reviewer: continue
        first = cs[0]['body']; layer = '설계' if first.startswith('[설계 리뷰]') else '코드'
        replies = [c['body'] for c in cs[1:] if c['author']['login'] == author]
        outcome = 'open'
        if any(ACCEPT.search(r) for r in replies): outcome = 'accepted'
        if any(REBUT.search(r) for r in replies) and outcome != 'accepted': outcome = 'rebutted'
        items.append({'pr': pr, 'file': t['path'], 'line': t['line'], 'layer': layer, 'claim': re.sub(r'^\[[^\]]+\]\s*', '', first)[:300], 'outcome': outcome, 'resolved': t['isResolved'], 'n_replies': len(replies)})
    for it in items: it['source'] = 'posted'
    for f in (local or []):
        line = int(f.get('line') or 0)
        posted = next((it for it in items if it['file'] == f.get('file') and it['line'] and abs(int(it['line']) - line) <= 3), None)
        if posted: posted['local_status'] = f.get('status'); posted['local_id'] = f.get('id'); continue
        items.append({'pr': pr, 'file': f.get('file'), 'line': line, 'layer': f.get('layer', '코드'), 'claim': (f.get('claim') or '')[:300],
                      'outcome': 'not-posted', 'resolved': False, 'n_replies': 0, 'source': 'local', 'status': f.get('status'), 'severity': f.get('severity'),
                      'importance': f.get('importance'), 'category': f.get('category'), 'rule_ids': f.get('rule_ids') or []})
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, f'PR-{pr}.json'); json.dump(items, open(p, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    return p
