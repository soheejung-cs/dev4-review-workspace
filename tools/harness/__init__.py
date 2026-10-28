"""dev4 harness — CodeGraph(Metis 방식) + 아키텍처 유추/리스크(Azure 방식) 융합.
두 파이프라인(design / review)이 같은 CodeGraph·ContextPack·Adjudicator 를 공유한다.

인터프리터: 세 컨테이너의 기본 python3 은 3.6.8 이라 dataclasses 가 없어 run·implement_pack 등이 import 에서 죽는다.
3.7 미만으로 불리면 PATH 의 python3.13~3.7 중 **쓸 수 있는** 가장 새 것으로 같은 명령을 다시 실행한다(/proc/self/cmdline 그대로) —
스킬의 `python3 -m tools.harness.*` 를 그대로 쓰게. '쓸 수 있다' = 후보로 `-c 'import yaml'` 이 성공한다(H-42: .51 의 3.11 에는
pip·PyYAML 이 없어 재실행된 `run` 이 `import yaml` 에서 죽었다). yaml·jsonschema 를 다 가진 후보를 먼저, 없으면 yaml 만 가진 후보를 고른다
(jsonschema 는 `--findings` 판정에서만 쓴다). 어느 후보도 못 쓰면 지금 인터프리터로 진행하고(unitseam·unit_regress·post_review 는
3.6 에서도 돈다) 건너뛴 후보와 설치 방법을 stderr 에 한 줄씩 남긴다. DEV4_HARNESS_REEXEC=1 이면 재실행하지 않는다(무한 exec 방지 겸 수동 고정).
"""
import sys as _sys

# 후보가 갖춰야 하는 모듈: yaml 은 pipeline 을 import 하는 순간 필요(없으면 run 이 죽는다), jsonschema 는 --findings 판정 때만
_REQUIRED = ('yaml',)
_PREFERRED = ('jsonschema',)
# 3.7 = dataclasses 가 들어온 버전, 3.13 = 이 리포에서 확인한 가장 새 판
_MINORS = range(13, 6, -1)
_PROBE_TIMEOUT = 20   # 초 — 인터프리터 기동 + import 한 번. NFS 홈에서 느린 첫 기동도 넉넉히


def _imports_ok(py, mods):
    """py 로 mods 를 import 할 수 있나 — 자식 프로세스로 확인한다(이 프로세스의 sys.path 와 무관하게)."""
    import subprocess
    try:
        r = subprocess.run([py, '-c', 'import ' + ', '.join(mods)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=_PROBE_TIMEOUT)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0


def _pick_interpreter(which=None, imports_ok=None):
    """(고른 인터프리터 경로 또는 None, 건너뛴 후보 [(경로, 없는 모듈)]). 새 판부터 본다 —
    필수+선호 모듈을 다 가진 첫 후보, 없으면 필수만 가진 첫 후보, 그것도 없으면 None(지금 인터프리터로 진행)."""
    import shutil
    which = which or shutil.which
    imports_ok = imports_ok or _imports_ok
    cands = [p for p in (which('python3.%d' % m) for m in _MINORS) if p]
    fallback, skipped = None, []
    for py in cands:
        if imports_ok(py, _REQUIRED + _PREFERRED):   # 흔한 경우(다 갖춤)는 자식 프로세스 한 번으로 끝난다
            return py, skipped
        if imports_ok(py, _REQUIRED):
            fallback = fallback or py
            skipped.append((py, _PREFERRED))
        else:
            skipped.append((py, _REQUIRED))
    return fallback, skipped


if _sys.version_info < (3, 7):
    import os as _os
    # 재실행 뒤에도 3.7 미만이면(PATH 이상) 다시 돌지 않는다 — 무한 exec 방지
    if not _os.environ.get('DEV4_HARNESS_REEXEC') and _os.path.exists('/proc/self/cmdline'):
        _py, _skipped = _pick_interpreter()
        for _p, _mods in _skipped:
            _sys.stderr.write('tools.harness: %s 에 %s 이(가) 없다%s — 설치: `%s -m pip install --user %s`'
                              '(pip 이 없으면 `%s -m ensurepip --user` 먼저)\n'
                              % (_p, ', '.join(_mods), ' (이것으로 진행, --findings 판정은 죽는다)' if _p == _py else ' (건너뜀)',
                                 _p, ' '.join('pyyaml' if m == 'yaml' else m for m in _mods), _p))
        if _py:
            with open('/proc/self/cmdline', 'rb') as _f:
                _argv = [a.decode() for a in _f.read().split(b'\0') if a]
            _os.environ['DEV4_HARNESS_REEXEC'] = '1'
            _sys.stderr.flush()   # 3.6 의 비대화형 stderr 는 블록 버퍼 — exec 전에 비우지 않으면 위 안내가 사라진다
            _os.execv(_py, [_py] + _argv[1:])
        elif _skipped:
            _sys.stderr.write('tools.harness: 쓸 수 있는 python3.7+ 가 없어 %s 로 진행 — run·implement_pack 은 dataclasses 가 없으면 '
                              'import 에서 죽는다\n' % _sys.version.split()[0])
