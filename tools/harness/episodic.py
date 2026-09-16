"""Episodic memory: 게시된 리뷰 코멘트와 작성자 응답을 examples/episodic/PR-<n>.json 으로 적재한다.
outcome: accepted(작성자가 수정/동의) | rebutted(반박) | open. 다음 리뷰의 context pack 우선순위 6 으로 들어간다."""
import json, os, re, subprocess
from typing import Dict, List

ACCEPT = re.compile(r'맞습니다|수정했습니다|반영했|동의합니다|고쳤습니다|fixed|agreed|done', re.I)
REBUT = re.compile(r'아닙니다|기존 설계|의도한|철회|not a bug|by design|오탐', re.I)

def collect(pr: int, reviewer: str, out_dir: str, repo: str = 'CUBRID/cubrid') -> str:
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
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, f'PR-{pr}.json'); json.dump(items, open(p, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    return p
