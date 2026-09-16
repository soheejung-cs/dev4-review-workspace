"""ServerSession: 검증 실행이 DB 를 띄워야 할 때 conf·databases.txt 를 백업/복원하고 종료를 보장한다(with 블록).
CTP 가 conf 를 바꿔 놓는 부작용(ha_mode/port)과 잔존 cub_* 프로세스를 하네스가 책임진다. 진행 판정에 pgrep -f 는 쓰지 않는다."""
import os, shutil, subprocess, time
from typing import List, Optional

class ServerSession:
    def __init__(self, db: str, cubrid_home: Optional[str] = None, port: Optional[int] = None):
        self.db = db; self.home = cubrid_home or os.environ.get('CUBRID') or os.path.expanduser('~/CUBRID'); self.port = port
        self.conf = os.path.join(self.home, 'conf'); self.bak = None; self.started = False
    def _snap(self):
        self.bak = self.conf + f'.bak_harness_{int(time.time())}'
        shutil.copytree(self.conf, self.bak)
        shutil.copy2(os.path.join(self.home, 'databases', 'databases.txt'), self.bak + '.databases.txt')
    def _restore(self):
        if not self.bak: return
        for fn in os.listdir(self.bak):
            shutil.copy2(os.path.join(self.bak, fn), os.path.join(self.conf, fn))
        shutil.copy2(self.bak + '.databases.txt', os.path.join(self.home, 'databases', 'databases.txt'))
        shutil.rmtree(self.bak, ignore_errors=True); os.remove(self.bak + '.databases.txt') if os.path.exists(self.bak + '.databases.txt') else None
    def __enter__(self):
        self._snap()
        if self.port:
            subprocess.run(['sed', '-i', f's/^cubrid_port_id=.*/cubrid_port_id={self.port}/', os.path.join(self.conf, 'cubrid.conf')])
        r = subprocess.run(['cubrid', 'server', 'start', self.db], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        if r.returncode: self._restore(); raise RuntimeError(r.stderr or r.stdout)
        self.started = True; return self
    def __exit__(self, *exc):
        try:
            if self.started: subprocess.run(['cubrid', 'server', 'stop', self.db], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        finally:
            self._restore(); cleanup_leftovers(owned_only=True)
        return False

def cleanup_leftovers(owned_only: bool = True) -> List[str]:
    """이 사용자 소유의 잔존 cub_cas/cub_server 중 부모가 죽은(orphan) 것만 정리. 판정은 pgrep -x(실행 파일명) 로만."""
    killed = []
    for exe in ('cub_cas', 'cub_server'):
        r = subprocess.run(['pgrep', '-x', '-u', str(os.getuid()), exe], stdout=subprocess.PIPE, universal_newlines=True)
        for pid in r.stdout.split():
            ppid = open(f'/proc/{pid}/stat').read().split()[3] if os.path.exists(f'/proc/{pid}/stat') else '0'
            if ppid == '1':  # 고아 = 부모(cub_master/broker)가 죽은 채 남은 것
                subprocess.run(['kill', pid]); killed.append(f'{exe}:{pid}')
    return killed
