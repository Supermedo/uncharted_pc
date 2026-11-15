from pathlib import Path
import json,struct,subprocess,sys,os
root=Path(sys.argv[1]);exe=Path(sys.argv[2]);root.mkdir(parents=True,exist_ok=False)
def psf(items):
 keys=bytearray();values=bytearray();entries=bytearray()
 for key,value in items:
  k=len(keys);keys+=key.encode()+b'\0';off=len(values)
  maximum=None
  if isinstance(value,tuple):value,maximum=value
  fmt=0x404 if isinstance(value,int) else 0x204
  value=struct.pack('<I',value) if isinstance(value,int) else value.encode()+b'\0'
  maximum=maximum or len(value)
  entries+=struct.pack('<HHIII',k,fmt,len(value),maximum,off);values+=value+bytes(maximum-len(value))
  values+=b'\0'*(-len(values)%4)
 kt=20+len(entries);dt=(kt+len(keys)+3)&~3
 return bytearray(struct.pack('<5I',0x46535000,0x101,kt,dt,len(items))+entries+keys+b'\0'*(dt-kt-len(keys))+values)
def values(tid='CUSA02343',cid=None,category='ac'):
 return [('TITLE_ID',tid),('CONTENT_ID',cid or 'EP9000-'+tid+'_00-NATHANDRAKEPACK1'),('CATEGORY',category)]
cases=[]
def add(name,data,expected=0,title=2343):cases.append((name,data,expected,title))
add('missing',None)
good=psf(values());add('valid-eu',good,1)
add('valid-us',psf(values('CUSA02320','UP9000-CUSA02320_00-NATHANDRAKEPACK1')),1,2320)
add('wrong-title',psf(values('CUSA02320')))
add('wrong-category',psf(values(category='gd')))
add('wrong-label',psf(values(cid='EP9000-CUSA02343_00-OTHERCONTENT0001')))
add('duplicate-field',psf(values()+[('TITLE_ID','CUSA02343')]))
add('missing-field',psf(values()[:2]))
add('empty',b'');add('oversize',good+bytes(65537-len(good)));add('size-boundary',good+bytes(65536-len(good)),1);add('truncated',good[:27])
for name,offset,fmt,value in [('bad-magic',0,'I',0),('bad-version',4,'I',0),('bad-keys',8,'I',0xffffffff),('bad-values',12,'I',0xffffffff),('bad-count',16,'I',0xffffffff),('bad-key-offset',20,'H',0xffff),('bad-length',24,'I',0xffffffff),('bad-value-offset',32,'I',0xffffffff),('wrong-format',22,'H',0)]:
 b=good.copy();struct.pack_into('<'+fmt,b,offset,value);add(name,b)
add('bad-publisher',psf(values(cid='ep9000-CUSA02343_00-NATHANDRAKEPACK1')))
add('content-title-mismatch',psf(values(cid='EP9000-CUSA02320_00-NATHANDRAKEPACK1')))
add('missing-title',psf(values()[1:]));add('missing-content',psf([values()[0],values()[2]]))
add('zero-title',good,0,0);add('large-title',good,0,100000)
b=good.copy();struct.pack_into('<II',b,24,99999,99999);add('length-and-maximum-overflow',b)
b=good.copy();struct.pack_into('<I',b,28,99999);add('maximum-overflow',b)
kt,dt=struct.unpack_from('<II',good,8)
b=good.copy();b[kt:dt]=b'X'*(dt-kt);add('unterminated-key',b)
add('truncated-key-table',good[:dt-1]);add('truncated-value-table',good[:-5])
b=psf([('XTITLE_ID','CUSA02343'),*values()[1:]]);struct.pack_into('<H',b,20,1);add('mid-key',b)
add('duplicate-category',psf(values()+[('CATEGORY','ac')]))
add('duplicate-content',psf(values()+[values()[1]]))
add('short-prefix',psf(values(cid='EP900-CUSA02343_00-NATHANDRAKEPACK1')))
b=good.copy();struct.pack_into('<I',b,8,dt);add('keys-equal-values',b)
b=good.copy();b[dt+len('CUSA02343')]=88;add('nonterminated-title',b)
b=good.copy();off=len(good)-dt;struct.pack_into('<III',b,24,1,1,off);add('offset-at-eof-plus-one',b)
b=good.copy();struct.pack_into('<II',b,24,len(good)-dt+1,len(good)-dt+1);add('length-one-past-eof',b)
add('realistic-padded-extra-keys',psf([('EXTRA_BEFORE',7),('TITLE_ID',('CUSA02343',16)),('EXTRA_BETWEEN','extra'),('CONTENT_ID',('EP9000-CUSA02343_00-NATHANDRAKEPACK1',48)),('CATEGORY',('ac',4)),('EXTRA_AFTER',5),('EXTRA_AFTER',6)]),1)
add('invalid-publisher-form',psf(values(cid='AAAAAA-CUSA02343_00-NATHANDRAKEPACK1')))
b=psf([('IGNORED','x'),*values()]);v=struct.unpack_from('<I',b,12)[0];struct.pack_into('<III',b,24,2,2,len(b)-v+1);add('unrelated-offset-past-eof',b)
b=psf([('IGNORED','x'),*values()]);v=struct.unpack_from('<I',b,12)[0];struct.pack_into('<II',b,24,len(b)-v+1,len(b)-v+1);add('unrelated-length-past-eof',b)
results=[]
for name,data,expected,title in cases:
 user=root/name;user.mkdir()
 if data is not None:
  target=user/'addcont'/('CUSA%05d'%title)/'NATHANDRAKEPACK1'/'sce_sys/param.sfo';target.parent.mkdir(parents=True);target.write_bytes(data)
 result=subprocess.run([str(exe),str(user),str(expected),str(title)],capture_output=True,text=True,timeout=5)
 results.append({'case':name,'expected':expected,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
 if not result.returncode and ('API PASS' not in result.stdout or ('installed=%d'%expected) not in result.stdout): results[-1]['returncode']=99
for name,arg,title,code in [('null-user','NULL',2343,0),('empty-user','EMPTY',2343,0),('nonzero-service',str(root/'valid-eu'),2343,21)]:
 env=os.environ.copy()
 if code:env['TEST_BAD_SERVICE']='1'
 result=subprocess.run([str(exe),arg,'1' if code else '0',str(title)],capture_output=True,text=True,env=env,timeout=5)
 results.append({'case':name,'returncode':0 if result.returncode==code and (not code or 'nonzero AppContent service label unsupported' in result.stderr) else 99,'stdout':result.stdout,'stderr':result.stderr})
env=os.environ.copy();env['TEST_REMOVE_METADATA']='1'
result=subprocess.run([str(exe),str(root/'valid-eu'),'1','2343'],capture_output=True,text=True,env=env,timeout=5)
results.append({'case':'reload-redetect-removed-metadata','returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
for name,arg,flag,code,message in [('trial-sku',str(root/'size-boundary'),'TEST_TRIAL',0,'API PASS'),('path-overflow','x'*4096,'',0,'API PASS'),('before-load',str(root),'TEST_BEFORE_LOAD',21,'AppContent called before module load'),('before-init',str(root),'TEST_BEFORE_INIT',21,'AppContent query before initialization')]:
 env=os.environ.copy()
 if flag:env[flag]='1'
 expected='1' if flag=='TEST_TRIAL' else '0'
 result=subprocess.run([str(exe),arg,expected,'2343'],capture_output=True,text=True,env=env,timeout=5)
 results.append({'case':name,'returncode':0 if result.returncode==code and message in result.stdout+result.stderr else 99,'stdout':result.stdout,'stderr':result.stderr})
(root/'results.json').write_text(json.dumps(results,indent=2));failed=[x['case'] for x in results if x['returncode']];print('FAIL' if failed else 'PASS',len(results),'parser/API fixtures',failed);sys.exit(bool(failed))
