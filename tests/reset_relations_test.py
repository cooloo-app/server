"""Offline reset preserves payloads and room ACL metadata; idempotent and fail closed."""
import importlib.util
import json
from pathlib import Path
import tempfile

script = Path(__file__).resolve().parents[1] / 'tools/reset_legacy_relations.py'
spec = importlib.util.spec_from_file_location('reset_relations', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    a, b, room = 'a'*64, 'b'*64, 'c'*64
    for id, kind, owner, slot in [(a,'user','',0),(b,'user',a,1),(room,'room',a,0)]:
        directory = root/'rooms'/id
        directory.mkdir(parents=True)
        obj = dict(id=id,type=kind,nickname='test',owner=owner,slot=slot)
        if kind == 'room':
            obj['access'] = 1
        (directory/'.meta').write_text(json.dumps(obj,separators=(',',':'))+'\n')
        (directory/'messages.jsonl').write_bytes(b'preserve exact history\n')
    (root/'identity').write_bytes(b'disposable key fixture')
    grant = root/'authz'/a/b
    grant.parent.mkdir(parents=True)
    grant.write_text('DELEGATE-1 3\n')
    originals = {p.relative_to(root):p.read_bytes() for p in root.rglob('*') if p.is_file()}
    assert module.reset(root)['detached_users'] == 1
    assert all((root/p).read_bytes()==v for p,v in originals.items())
    assert module.reset(root,True) == dict(applied=True,detached_users=1,removed_legacy_grants=1)
    for p,v in originals.items():
        if p.parts[0]=='authz' or p==Path('rooms')/b/'.meta':
            continue
        assert (root/p).read_bytes()==v, p
    meta=json.loads((root/'rooms'/b/'.meta').read_text())
    assert meta['owner']=='' and meta['slot']==0
    assert not (root/'authz').exists()
    assert module.reset(root,True)['detached_users']==0
    (root/'relations').mkdir()
    (root/'relations'/b).write_text(f'RELATION-1 {a} 1 1 3\n')
    try:
        module.reset(root,True)
    except ValueError:
        pass
    else:
        raise AssertionError('new relations must stop reset')
print('reset: dry run, exact data/key/ACL preservation, idempotency and new-relation refusal passed')
