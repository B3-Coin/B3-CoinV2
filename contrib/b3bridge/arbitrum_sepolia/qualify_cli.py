#!/usr/bin/env python3
"""Disconnected process controls using a real native Gloas journal/preimage.

Head/account witnesses are intentionally invalid negative controls. This never
claims a real public Arbitrum settlement or synthesizes an Ethereum anchor.
"""
import argparse,dataclasses,hashlib,json,os,subprocess,sys
from pathlib import Path
BRIDGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(BRIDGE));sys.path.insert(0,str(BRIDGE/'arbitrum_history'))
from capture_eth_receipts_fixture import keccak256
from proof_bundle import Assertion,AccountWitness,StorageWitness,BlockWitness,Proof,encode_proof

def sha(data):return hashlib.sha256(data).hexdigest()
def run(a):
 out=a.output.resolve();out.mkdir(mode=0o700,parents=True,exist_ok=False)
 policy=a.policy.read_bytes();journal=a.journal.read_bytes();raw=a.execution_header.read_bytes()
 for name,data in [('policy.json',policy),('proofs.journal',journal),('execution-header.rlp',raw)]:
  (out/name).write_bytes(data);(out/name).chmod(0o600)
 before={name:sha((out/name).read_bytes()) for name in ['policy.json','proofs.journal','execution-header.rlp']}
 pins=json.loads((BRIDGE/'arbitrum_sepolia/provenance.json').read_text())['accounts']
 assertion=Assertion(bytes([1])*32,bytes([2])*32,bytes([3])*32,1,0,1,bytes([4])*32,bytes([5])*32)
 state=assertion.block_hash+assertion.send_root+(1).to_bytes(32,'big')+bytes(32)+(1).to_bytes(32,'big')+assertion.end_history_root
 assertion_hash=keccak256(assertion.parent_assertion+keccak256(state)+assertion.inbox_accumulator)
 slot=keccak256(assertion_hash+(117).to_bytes(32,'big'))
 accounts=[]
 for index,record in enumerate(pins.values()):
  keys=[bytes.fromhex(s[2:]) for s in record['storage']]
  if index==0:keys += [(116).to_bytes(32,'big'),slot]
  accounts.append(AccountWitness(bytes.fromhex(record['address'][2:]),(),tuple(StorageWitness(k,()) for k in keys)))
 block=BlockWitness(b'\xc0') # transport-only header; invalid witnesses fail first
 (out/'head0.bin').write_bytes(encode_proof(Proof(assertion,tuple(accounts[:16]),(block,))))
 (out/'head1.bin').write_bytes(encode_proof(Proof(assertion,tuple(accounts[16:]),(block,))))
 (out/'head1-wrong.bin').write_bytes(encode_proof(Proof(dataclasses.replace(assertion,inbox_position=2),tuple(accounts[16:]),(block,))))
 query={'address':pins['rollup']['address'],'nodes':[],'storage':[{'slot':'0x'+s.slot.hex(),'nodes':[]} for s in accounts[0].storage]}
 (out/'query.json').write_text(json.dumps(query))
 (out/'query-flag.json').write_text(json.dumps(dict(query,verified=True)))
 (out/'query-duplicate.json').write_text('{"address":"'+query['address']+'",'+json.dumps(query)[1:])
 mutated=bytearray(raw);mutated[-1]^=1;(out/'bad-el.rlp').write_bytes(mutated)
 mutated=bytearray(journal);mutated[0]^=1;(out/'bad.journal').write_bytes(mutated);(out/'bad.journal').chmod(0o600)
 (out/'truncated.journal').write_bytes(journal[:-1]);(out/'truncated.journal').chmod(0o600)
 (out/'policy-mainnet.json').write_text(json.dumps(dict(json.loads(policy),chain_id=1)));(out/'policy-mainnet.json').chmod(0o600)
 (out/'linked.journal').symlink_to(out/'proofs.journal')
 common=['--policy',str(out/'policy.json'),'--journal',str(out/'proofs.journal'),'--execution-header',str(out/'execution-header.rlp')]
 full=common+['--head-proof',str(out/'head0.bin'),'--head-proof-1',str(out/'head1.bin'),'--ancestor-pages',str(out/'unused-pages.json'),'--objects',str(out/'objects'),'--history-proof',str(out/'unused-history.bin'),'--previous-height','0','--previous-hash','0x'+'11'*32,'--to-height','1','--to-hash','0x'+'22'*32]
 query_args=common+['--query-latest','--rollup-witness',str(out/'query.json')]
 def change(args,key,value):
  result=list(args);result[result.index(key)+1]=str(value);return result
 cases=[('actual-ethereum-unproved-17-accounts',full,'BAD_ACCOUNT'),('actual-ethereum-unproved-query',query_args,'BAD_ACCOUNT'),
  ('altered-execution-preimage',change(full,'--execution-header',out/'bad-el.rlp'),'STALE_OR_BAD_EXECUTION_PREIMAGE'),
  ('journal-magic',change(full,'--journal',out/'bad.journal'),'INVALID_INPUT_OR_LIMIT'),
  ('journal-truncation',change(full,'--journal',out/'truncated.journal'),'INVALID_INPUT_OR_LIMIT'),
  ('missing-journal-not-created',change(full,'--journal',out/'missing.journal'),'INVALID_INPUT_OR_LIMIT'),
  ('journal-symlink',change(full,'--journal',out/'linked.journal'),'INVALID_INPUT_OR_LIMIT'),
  ('wrong-network-policy',change(full,'--policy',out/'policy-mainnet.json'),'INVALID_INPUT_OR_LIMIT'),
  ('head-groups-different-assertions',change(full,'--head-proof-1',out/'head1-wrong.bin'),'INVALID_INPUT_OR_LIMIT'),
  ('query-extra-authority-flag',change(query_args,'--rollup-witness',out/'query-flag.json'),'INVALID_INPUT_OR_LIMIT'),
  ('query-duplicate-field',change(query_args,'--rollup-witness',out/'query-duplicate.json'),'INVALID_INPUT_OR_LIMIT'),
  ('fake-clock-refused',query_args+['--slot','1'],'INVALID_INPUT_OR_LIMIT'),
  ('verified-flag-refused',query_args+['--verified','true'],'INVALID_INPUT_OR_LIMIT')]
 records=[]
 for name,args,expected in cases:
  result=subprocess.run([str(a.verifier),*args],capture_output=True,timeout=30)
  try:value=json.loads(result.stdout)
  except Exception as error:raise AssertionError('missing structured result: '+name) from error
  assert result.returncode==2 and value['result']==expected,(name,result.returncode,value)
  for flag in ['mainnet_enabled','arbitrum_authority','execution_verified','writes_enabled','sepolia_contract_settlement_verified','commitments_match']:assert value.get(flag) is False,(name,flag,value)
  case=out/name;case.mkdir(mode=0o700);(case/'stdout.json').write_bytes(result.stdout);(case/'stderr.txt').write_bytes(result.stderr)
  records.append({'name':name,'exit_code':result.returncode,'result':value['result'],'diagnostic_stage':value.get('diagnostic_stage'),'passed':True})
 after={name:sha((out/name).read_bytes()) for name in before};assert before==after;assert not (out/'missing.journal').exists()
 report={'format':'b3-sepolia-native-process-controls/1','passed':True,'cases':records,'case_count':len(records),'input_bytes_unchanged':True,'real_ethereum_journal_and_preimage':True,'public_arbitrum_settlement_tested':False,'all_heads_and_accounts_are_negative_controls':True,'mainnet_enabled':False,'binary_sha256':sha(a.verifier.read_bytes())}
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':True,'cases':len(records),'report':str(out/'report.json')}))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verifier',type=Path,required=True);p.add_argument('--policy',type=Path,required=True);p.add_argument('--journal',type=Path,required=True);p.add_argument('--execution-header',type=Path,required=True);p.add_argument('--output',type=Path,required=True);run(p.parse_args())
