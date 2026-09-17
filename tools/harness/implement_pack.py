#!/usr/bin/env python3
"""구현용 컨텍스트 팩 — diff 가 아니라 심볼·키워드·파일로 '손댈 후보 함수' 를 고른다.
사용: python3 -m tools.harness.implement_pack --key CBRD-27369 [--repo ~/dev/sources/cubrid]
        [--symbols fn1,fn2] [--files src/a.c,src/b.c] [--grep "error text,another"] [--out ~/dev/utils/harness-out/impl/<key>]
산출: implement_request[.batchN].md (지시 + plan 스키마 + arch 요약 + 팩), codegraph.sqlite3, arch.json, manifest.json
LLM(세션)은 이 팩만 읽고 plan.json 을 쓴다 — 팩 밖 코드는 "없음" (리뷰 하네스와 같은 규율)."""
import argparse, json, os, re, subprocess, sys, time
from . import codegraph as CG, context_pack as CP, arch_infer as AI

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_EXT = ('.c', '.cpp', '.h', '.hpp', '.cc')

def sh(*a, cwd=None):
    return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=cwd).stdout

def git_grep(repo, pat, word=True):
    args = ['git', '-C', repo, 'grep', '-n', '-I'] + (['-w'] if word else []) + ['-e', pat, '--', 'src/']
    out = {}
    for line in sh(*args).splitlines():
        f, ln, _ = line.split(':', 2)
        if f.endswith(SRC_EXT): out.setdefault(f, []).append(int(ln))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--key', required=True, help='CBRD-n (산출 디렉터리·문서 키)')
    ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--symbols', default='', help='후보 함수 이름, 콤마 구분 (정의 위치를 찾아 그 함수 전체를 팩에)')
    ap.add_argument('--files', default='', help='후보 파일, 콤마 구분 (그 파일의 함수 중 --grep/--symbols 에 걸린 것; 없으면 파일 전체는 넣지 않고 그래프 범위로만)')
    ap.add_argument('--grep', default='', help='본문 키워드(에러 코드·메시지·필드명), 콤마 구분 — 그 줄을 포함하는 함수를 후보로')
    ap.add_argument('--out', default='')
    ap.add_argument('--budget', type=int, default=12000)
    ap.add_argument('--max-fns', type=int, default=24)
    a = ap.parse_args()
    out = a.out or os.path.expanduser(f'~/dev/utils/harness-out/impl/{a.key}'); os.makedirs(out, exist_ok=True)
    log = lambda m: print(f'[impl-pack] {m}', file=sys.stderr)
    repo = a.repo
    symbols = [s.strip() for s in a.symbols.split(',') if s.strip()]
    greps = [s.strip() for s in a.grep.split(',') if s.strip()]
    files = {f.strip() for f in a.files.split(',') if f.strip()}

    # 1) 후보 줄 수집: 심볼 정의( 'name (' 로 시작하는 줄 ) + 키워드 줄
    hits = {}
    for sym in symbols:
        for f, lns in git_grep(repo, sym).items():
            for ln in lns: hits.setdefault(f, set()).add(ln)
    for kw in greps:
        for f, lns in git_grep(repo, kw, word=False).items():
            for ln in lns: hits.setdefault(f, set()).add(ln)
    for f in files: hits.setdefault(f, set())
    if not hits: log('후보 없음 — --symbols/--grep/--files 중 하나는 실제로 소스에 있어야 한다'); sys.exit(2)

    # 2) 그래프 범위 = 후보 파일 + 같은 디렉터리 (리뷰 하네스의 changed+1 과 같은 규칙)
    scope = set(hits)
    for f in list(hits):
        d = os.path.dirname(f); dd = os.path.join(repo, d)
        if os.path.isdir(dd): scope |= {os.path.join(d, fn) for fn in os.listdir(dd) if fn.endswith(SRC_EXT)}
    g = CG.CodeGraph.build(repo, sorted(scope), os.path.join(out, 'codegraph.sqlite3'), log=log)

    # 3) 후보 함수: 심볼은 '정의' 만(호출 줄 제외), 키워드는 그 줄을 포함하는 함수
    changed = {}; fids = []
    for f, lns in sorted(hits.items()):
        for fn in g.functions_in(f, sorted(lns)):
            is_sym_def = fn.name in symbols
            is_kw = any(ln in lns for ln in range(fn.start_line, fn.end_line + 1)) and (greps or not symbols)
            if is_sym_def or is_kw or (fn.name in symbols):
                if fn.fid in fids: continue
                fids.append(fn.fid); changed.setdefault(fn.file, []).extend(range(fn.start_line, fn.end_line + 1))
    # 심볼 정의가 hits 파일에 없을 수도(함수 정의 줄 패턴): functions 테이블에서 이름으로 보강
    for sym in symbols:
        for (fid, file, s, e) in g.db.execute('SELECT fid,file,start_line,end_line FROM functions WHERE name=?', (sym,)):
            if fid not in fids: fids.append(fid); changed.setdefault(file, []).extend(range(s, e + 1))
    if len(fids) > a.max_fns:
        log(f'후보 {len(fids)}개 > {a.max_fns} — 키워드가 넓다. 앞 {a.max_fns}개만 넣고 나머지는 목록으로'); extra = fids[a.max_fns:]; fids = fids[:a.max_fns]
        changed = {}
        for fid in fids:
            fn = g.function(fid); changed.setdefault(fn.file, []).extend(range(fn.start_line, fn.end_line + 1))
    else: extra = []
    log(f'candidates: {len(fids)} functions in {len(changed)} files (graph {len(scope)} files, complete={g.complete()})')

    # 4) 팩 + arch
    pack = CP.build(repo, g, '', os.path.join(ROOT, 'rules'), a.budget, os.path.join(ROOT, 'examples'), changed=changed, label='candidate function')
    bs = CP.batches(pack, a.budget)
    arch = AI.infer(g, fids); risks = AI.detect_risks(arch, g, fids)
    json.dump({'architecture': arch, 'risks': risks}, open(os.path.join(out, 'arch.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(out, 'arch.mmd'), 'w').write(AI.to_mermaid(arch, risks))
    schema = open(os.path.join(ROOT, 'harness', 'schemas', 'plan.json'), encoding='utf-8').read()
    head = f"""# implement_request — {a.key} (repo HEAD {sh('git','-C',repo,'rev-parse','--short=9','HEAD').strip()}, {time.strftime('%Y-%m-%d')})

## 지시 (구현 계획 — 코드를 쓰기 전에)
1. 아래 컨텍스트 팩만 읽는다. 팩에 없는 코드는 "없음" 으로 적고 추측하지 않는다(더 필요하면 `--symbols/--grep` 을 넓혀 팩을 다시 만든다).
2. **영역 판정 4단계**를 먼저 문장으로: ① 컴포넌트 [A]~[I] ② 불변조건 5개(INV-1~5) 중 흔드는 것 ③ 임계도 Tier-0/1/2 ④ 공유·격리(공유면 불변식·허용 동시성·완화 3요소). "이건 [X] 영역 작업이므로 ~를 고려해야 한다" — 불변조건을 흔들면 구현으로 가지 말고 설계문서로 돌아간다.
3. **대안 2개 이상**과 채택 이유(표). PG/InnoDB/Oracle 은 같은 문제를 어떻게 하나 한 줄.
4. **바꿀 함수 목록**은 팩 안의 `candidate function` 중에서만. 함수마다 무엇을·왜·관문 계약(무엇을 잡고 어디서 놓나, 에러 경로 포함). 팩 밖 함수를 바꿔야 하면 `needs_more_context` 에 이름을 적고 멈춘다.
5. **TC 시나리오**(재현 → 기대) 와 **회귀 위험**(이 변경으로 달라질 수 있는 기존 동작). 성능 경로면 성능규칙집 ID 로 지킬 규칙을 적는다.
6. 산출은 `plan.json` 하나(스키마 아래). 사용자가 plan 을 승인한 뒤에만 코드를 고친다.

## 아키텍처 유추 (arch.json 요약)
- candidate layers: {arch.get('touched_layers')}
- risks: {json.dumps(risks, ensure_ascii=False)[:1500]}
{('- 후보 초과로 팩에 못 넣은 함수: ' + ', '.join(extra)) if extra else ''}

## plan 스키마 (harness/schemas/plan.json)
```json
{schema}
```
"""
    written = []
    for i, b in enumerate(bs):
        name = 'implement_request.md' if len(bs) == 1 else f'implement_request.batch{i+1}.md'
        open(os.path.join(out, name), 'w', encoding='utf-8').write(head + f"\n## 컨텍스트 팩 ({i+1}/{len(bs)})\n" + b.render()); written.append(name)
    json.dump({'key': a.key, 'repo_head': sh('git','-C',repo,'rev-parse','HEAD').strip(), 'symbols': symbols, 'grep': greps, 'files': sorted(files),
               'candidate_fids': fids, 'n_files_parsed': len(scope), 'codegraph_complete': g.complete(), 'n_batches': len(bs),
               'harness_git': sh('git','-C',ROOT,'rev-parse','--short','HEAD').strip(), 'created': time.strftime('%Y-%m-%dT%H:%M:%S')},
              open(os.path.join(out, 'manifest.json'), 'w'), ensure_ascii=False, indent=1)
    log(f'written: {written}'); print(out)

if __name__ == '__main__': main()
