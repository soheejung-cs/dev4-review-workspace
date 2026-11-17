#!/usr/bin/env python3
"""eval_leak_check — 평가 시도의 에이전트 기록(transcript JSONL)에서 정답이 있을 수 있는 파일을 연 흔적을 찾는다(harness/평가.md §3).

계기: 2026-11-17 첫 기준점에서 판정 에이전트 둘이 팩 밖의 평가 과제 파일·예전 게시 코멘트·소스 노트 원본을 직접 열었다.
사용: python3 -m tools.harness.eval_leak_check --pr <pr> [--jira CBRD-n] <transcript.jsonl>…
보는 것: 도구 호출(tool_use) 입력 안의 경로만 — 프롬프트·팩 본문에 같은 글자가 있는 것은 세지 않는다.
  harness/evals · 평가.md · 놓친지적-반성 · 리뷰에서-배운것 · examples/episodic/PR-<pr> · claude-workspace/projects/<jira> ·
  dev4-ai-source/modules · dev4-ai-source/staging · unit/ 아래 suite(origin 이 그 PR 일 수 있다)
출력: 기록마다 `clean` 또는 `LEAK <종류>=<횟수>…`. 종료 코드: 하나라도 LEAK 이면 1. 읽기만 한다.
"""
import argparse
import json
import re
import sys


def patterns(pr, jira):
    p = {'evals': r'harness/evals|평가\.md', 'reflection': r'놓친지적-반성|리뷰에서-배운것',
         'episodic': r'examples/episodic/PR-%s\b' % pr, 'notes': r'dev4-ai-source/(modules|staging)',
         'unit': r'dev4-review-workspace/unit/|(^|[\s"\'])unit/[a-z]+/'}
    if jira:
        p['project'] = r'projects/%s\b' % re.escape(jira)
    return {k: re.compile(v) for k, v in p.items()}


def tool_inputs(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                o = json.loads(line)
            except ValueError:
                continue
            msg = o.get('message') or {}
            for c in msg.get('content') or []:
                if isinstance(c, dict) and c.get('type') == 'tool_use':
                    yield json.dumps(c.get('input', {}), ensure_ascii=False)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pr', required=True)
    ap.add_argument('--jira', default='')
    ap.add_argument('transcripts', nargs='+')
    a = ap.parse_args(argv)
    pats, bad = patterns(a.pr, a.jira), 0
    for t in a.transcripts:
        hits = {}
        for inp in tool_inputs(t):
            for k, rx in pats.items():
                if rx.search(inp):
                    hits[k] = hits.get(k, 0) + 1
        if hits:
            bad += 1
            print('LEAK  %s  %s' % (t, ' '.join('%s=%d' % kv for kv in sorted(hits.items()))))
        else:
            print('clean %s' % t)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
