#!/usr/bin/env python3
"""shell TC 분류·인덱스 생성기 (휴리스틱).
cubrid-testcases-private-ex 의 shell*/ 아래 cases/*.sh 를 훑어 케이스마다
  - 분류(최상위 디렉터리 라벨), 이슈 키(CBRD-n / bug_bts_n / issue_n), 기능 태그(키워드), 사용 유틸리티, 첫 주석
을 뽑아 tests/shell-tc-index/index.json + index.md(분류 요약) + by-tag.md 를 쓴다.
사용: tc_index_build.py [--repo ~/dev/sources/cubrid-testcases-private-ex] [--out tests/shell-tc-index]
"""
import argparse, json, os, re, sys, collections, subprocess, datetime
TAGS = {  # 태그: 케이스 본문(sh+sql, 소문자)에서 찾는 정규식
 'index':      r'\bcreate (unique |reverse )*index\b|\bbtree|\bidx\b|use index|index skip|covering',
 'statistics': r'update statistics|histogram|analyze table|statistics',
 'lock':       r'\block(db| table|s?\b)|deadlock|lock_timeout|isolation level|lockdb',
 'mvcc':       r'\bmvcc\b|vacuum|snapshot',
 'recovery':   r'\brecover|checkpoint|\bkill -9|cub_server.*kill|crash|restoredb|backupdb|log_max_archives|archive',
 'backup':     r'backupdb|restoredb|restoreslave|backup',
 'loaddb':     r'\bloaddb\b|\bunloaddb\b|--data-file|--schema-file',
 'checkdb':    r'\bcheckdb\b|\bcompactdb\b|\bspacedb\b|\bdiagdb\b|\bvacuumdb\b',
 'ha':         r'\bha_mode|heartbeat|copylogdb|applylogdb|\bha\b|replication|changemode|\bslave\b|\bmaster\b',
 'broker':     r'\bbroker\b|cub_cas|\bcas\b|appl_server|shard|broker_port|access_control',
 'jdbc':       r'\bjdbc\b|\.java\b|javac|java -',
 'cci':        r'\bcci\b|cci_connect|gcc .*cci|libcascci',
 'plcsql':     r'plcsql|create (or replace )?(procedure|function)|pl/csql|\bcubrid pl\b|pl_server',
 'javasp':     r'javasp|java_stored|loadjava|create (or replace )?(procedure|function).*language java',
 'partition':  r'\bpartition',
 'trigger':    r'\btrigger\b',
 'serial':     r'\bserial\b|auto_increment|\.next_value|\.current_value',
 'regexp':     r'regexp|rlike|regexp_engine',
 'charset':    r'collat|charset|make_locale|cubrid_locales|utf8|euckr|iso88591|\blocale\b',
 'json':       r'\bjson_|\bjson\b',
 'view':       r'create (or replace )?view|\bvclass\b',
 'schema':     r'alter (table|class)|rename|drop (table|class)|add (column|attribute)|change (column|attribute)|\bcomment\b',
 'user-auth':  r'create user|grant |revoke |\bpassword\b|access_ip|acl',
 'query-plan': r'query_plan|recompile|xasl|plan cache|--plan|;plan|set optimization|opt level',
 'execution':  r'group by|order by|\bjoin\b|subquery|\bunion\b|\bmerge\b|cte\b|with recursive|analytic|over\s*\(|limit ',
 'temp-vol':   r'temp_vol|temp volume|addvoldb|--temp|sort_buffer|temp_file',
 'session':    r'\bsession\b|set system parameters|sysparam|@@|set names|client timezone|timezone',
 'utility':    r'cubrid (tranlist|killtran|paramdump|statdump|lockdb|plandump|renamedb|copydb|installdb|alterdbhost|genlocale|gen_tz|synccolldb|tz|addvoldb|optimizedb|applyinfo|checksumdb|flashback)\b',  # createdb/server start 같은 공용 골격은 제외
 'tranlist':   r'\btranlist\b|\bkilltran\b',
 'timezone':   r'timezone|\btz\b|gen_tz|tz_',
 'datatype':   r'\bblob\b|\bclob\b|\benum\b|\bbit\b|numeric\(|\bdatetime\b|\btimestamp\b|\bdouble\b|cast\(',
 'perf-load':  r'\bperf\b|tps|sysbench|ycsb|tpcc|tpch|benchmark',
}
MODULE_TAGS = {  # 소스 경로 → 태그 (tc_relevance 가 쓴다)
 r'src/storage/btree': ['index'], r'src/storage/statistics|src/optimizer/histogram|catalog_class': ['statistics','query-plan'],
 r'src/transaction/lock': ['lock'], r'src/transaction/(mvcc|log_)|vacuum': ['mvcc','recovery'],
 r'src/transaction/log_|log_recovery|log_manager': ['recovery'], r'src/storage/(heap|file_manager|page_buffer|disk_manager)': ['recovery','temp-vol','execution'],
 r'src/executables/(load|unload)': ['loaddb'], r'src/executables/(util|check|compact)': ['checkdb','utility'],
 r'src/executables/backup|src/transaction/boot': ['backup','recovery'], r'src/base/heartbeat|src/replication|src/executables/(copylog|applylog)': ['ha'],
 r'src/broker': ['broker','cci','jdbc'], r'src/jdbc|cubrid-jdbc': ['jdbc'], r'src/cci': ['cci'],
 r'src/sp|pl_engine|src/method|plcsql': ['plcsql','javasp'], r'partition': ['partition'], r'trigger': ['trigger'], r'serial': ['serial'],
 r'regex|string_regex': ['regexp'], r'locale|intl|charset|collation|src/base/tz': ['charset','timezone'], r'db_json|json': ['json'],
 r'src/object/(schema|class|authenticate)': ['schema','user-auth','view'], r'src/parser': ['schema','datatype','execution'],
 r'src/optimizer': ['query-plan','execution'], r'src/query/(xasl|query_manager|list_file|scan|fetch|query_executor)': ['execution','query-plan','temp-vol'],
 r'src/query/execute_(schema|statement)': ['schema','statistics','session'], r'src/session|src/base/system_parameter': ['session'],
 r'src/connection|src/communication': ['broker','session'],
}
ISSUE_RE = re.compile(r'(CBRD-\d+|cbrd_?\d+|bug_bts_\d+|issue_\d+|apricot_\d+|APRICOT-\d+|CUBRIDSUS-\d+)', re.I)
def read(p, n=200_000):
    try:
        with open(p, encoding='utf-8', errors='replace') as f: return f.read(n)
    except OSError: return ''
def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid-testcases-private-ex')); ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),'tests','shell-tc-index'))
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    rev = subprocess.run(['git','-C',a.repo,'rev-parse','--short','HEAD'],stdout=subprocess.PIPE,universal_newlines=True).stdout.strip()
    cases = []
    for suite in sorted(d for d in os.listdir(a.repo) if d.startswith('shell') and os.path.isdir(os.path.join(a.repo,d))):
        for root, dirs, files in os.walk(os.path.join(a.repo, suite)):
            if os.path.basename(root) != 'cases': continue
            case_dir = os.path.dirname(root); rel = os.path.relpath(case_dir, a.repo)
            shs = [f for f in files if f.endswith('.sh')]
            if not shs: continue
            text = ''.join(read(os.path.join(root,f)) for f in files if f.endswith(('.sh','.sql','.java','.c','.py')))
            low = text.lower()
            tags = [t for t, rx in TAGS.items() if re.search(rx, low)]
            issues = sorted({m.upper().replace('CBRD_','CBRD-') for m in ISSUE_RE.findall(rel + ' ' + text)})
            comment = next((l.strip('# ').strip() for l in read(os.path.join(root, shs[0])).splitlines() if l.startswith('#') and not l.startswith('#!') and re.search(r'[A-Za-z가-힣]{3}', l)), '')
            parts = rel.split('/'); label = parts[1] if len(parts) > 1 else parts[0]
            cases.append({'path': rel, 'suite': suite, 'group': label, 'sh': shs, 'tags': tags, 'issues': issues, 'comment': comment[:120],
                          'utilities': sorted(set(re.findall(r'cubrid (\w+)', low)) - {'server','service'})[:12]})
    with open(os.path.join(a.out,'index.json'),'w',encoding='utf-8') as f: json.dump({'repo': a.repo, 'rev': rev, 'generated': datetime.date.today().isoformat(), 'tags': list(TAGS), 'module_tags': MODULE_TAGS, 'cases': cases}, f, ensure_ascii=False, indent=0)
    bygroup = collections.defaultdict(list); bytag = collections.defaultdict(list)
    for c in cases:
        bygroup[(c['suite'], c['group'])].append(c)
        for t in c['tags']: bytag[t].append(c)
    L = [f"# shell TC 분류·인덱스 — {rev} ({datetime.date.today()})", '', f"케이스 {len(cases)}개 · 생성기 `tools/tc_index_build.py`(휴리스틱: 키워드 태그) · 판정 도구 `tools/tc_relevance.py`", '',
         '## 분류(디렉터리) 요약', '', '| suite | group | 케이스 | 주 태그(상위 5) | 이슈 키 예 |', '|---|---|---|---|---|']
    for (s,g), cs in sorted(bygroup.items()):
        tc = collections.Counter(t for c in cs for t in c['tags']).most_common(5)
        iss = sorted({i for c in cs for i in c['issues'] if i.startswith('CBRD')})[:3]
        L.append(f"| {s} | {g} | {len(cs)} | {', '.join(f'{t}({n})' for t,n in tc)} | {', '.join(iss)} |")
    L += ['', '## 태그 → 케이스 수', '', '| 태그 | 케이스 | 정의(정규식) |', '|---|---|---|'] + [f"| {t} | {len(bytag[t])} | `{TAGS[t][:70]}` |" for t in TAGS]
    L += ['', '## 태그 없는 케이스', '', f"{sum(1 for c in cases if not c['tags'])}개 — 판정 시 '알 수 없음(낮음, 근거 없음)' 으로 나온다. 목록은 `index.json` 에서 `tags==[]`."]
    open(os.path.join(a.out,'index.md'),'w',encoding='utf-8').write('\n'.join(L)+'\n')
    with open(os.path.join(a.out,'by-tag.md'),'w',encoding='utf-8') as f:
        f.write(f"# 태그별 케이스 목록 — {rev}\n\n")
        for t in TAGS:
            f.write(f"## {t} ({len(bytag[t])})\n\n"); f.write('\n'.join(f"- `{c['path']}` {' '.join(c['issues'][:2])} — {c['comment']}" for c in bytag[t]) + '\n\n')
    print(f"{len(cases)} cases, {len(bygroup)} groups, untagged {sum(1 for c in cases if not c['tags'])} -> {a.out}")
if __name__ == '__main__': main()
