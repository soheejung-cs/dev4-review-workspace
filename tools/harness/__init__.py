"""dev4 harness — CodeGraph(Metis 방식) + 아키텍처 유추/리스크(Azure 방식) 융합.
두 파이프라인(design / review)이 같은 CodeGraph·ContextPack·Adjudicator 를 공유한다.

인터프리터: 세 컨테이너의 기본 python3 은 3.6.8 이라 dataclasses 가 없어 run·implement_pack 등이 import 에서 죽는다.
3.7 미만으로 불리면 PATH 의 python3.13~3.7 중 가장 새 것으로 **같은 명령을 다시 실행**한다(/proc/self/cmdline 그대로) —
스킬의 `python3 -m tools.harness.*` 를 그대로 쓰게. 찾지 못하면 그대로 진행한다(unitseam·unit_regress·post_review 는 3.6 에서도 돈다).
"""
import sys as _sys

if _sys.version_info < (3, 7):
    import os as _os
    import shutil as _shutil
    # 재실행 뒤에도 3.7 미만이면(PATH 이상) 다시 돌지 않는다 — 무한 exec 방지
    if not _os.environ.get('DEV4_HARNESS_REEXEC') and _os.path.exists('/proc/self/cmdline'):
        # 3.7 = dataclasses 가 들어온 버전, 3.13 = 이 리포에서 확인한 가장 새 판
        _py = next((p for p in (_shutil.which('python3.%d' % m) for m in range(13, 6, -1)) if p), None)
        if _py:
            with open('/proc/self/cmdline', 'rb') as _f:
                _argv = [a.decode() for a in _f.read().split(b'\0') if a]
            _os.environ['DEV4_HARNESS_REEXEC'] = '1'
            _os.execv(_py, [_py] + _argv[1:])
