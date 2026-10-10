#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
import types, sys, io, contextlib
from unittest.mock import patch
P=Path(__file__).resolve().parents[1]/'examples/goodix521d-finger-monitor.py'
spec=importlib.util.spec_from_file_location('monitor',P)
m=importlib.util.module_from_spec(spec)
if P.exists(): spec.loader.exec_module(m)
class MonitorTests(unittest.TestCase):
 def test_implementation(self): self.assertTrue(hasattr(m,'PromptState'))
 def state(self): self.assertTrue(hasattr(m,'PromptState')); return m.PromptState()
 def test_complete_contact(self):
  s=self.state();self.assertEqual(s.update(False,False),'KEEP READER EMPTY until PLACE appears.')
  self.assertEqual(s.update(True,False),'PLACE your finger on the reader.')
  self.assertEqual(s.update(True,True),'HOLD your finger still.')
  self.assertEqual(s.update(False,True),'REMOVE your finger from the reader.')
  self.assertIsNone(s.update(False,True));self.assertEqual(s.update(False,False),'KEEP READER EMPTY until PLACE appears.')
 def test_initial_present_does_not_prompt_removal(self):
  s=self.state();self.assertIsNone(s.update(False,True));self.assertIsNone(s.update(False,True))
 def test_initial_hold_cannot_authorize_removal(self):
  s=self.state();s.update(True,True,initial=True);self.assertIsNone(s.update(False,True))
 def test_reset_discards_hold(self):
  s=self.state();s.update(True,True);s.reset();self.assertIsNone(s.update(False,True))
 def test_invalid_types_discard_hold(self):
  s=self.state();s.update(True,True)
  with self.assertRaises(ValueError): s.update(1,True)
  self.assertIsNone(s.update(False,True))
 def test_invalidation(self):
  s=self.state();s.update(True,True);s.invalidate()
  self.assertIsNone(s.update(False,True))
 def test_timeout_bound(self):
  self.assertTrue(hasattr(m,'timeout_value'))
  for n in ('0','901','x','-1'):
   with self.assertRaises(ValueError):m.timeout_value(n)
  self.assertEqual(m.timeout_value('900'),900)

 def test_readonly_main_queued_signals_and_timeout(self):
  self.run_fake(False)
 def test_owner_loss_stops_without_remove(self):
  self.run_fake(True)
 def run_fake(self, owner_loss):
  callbacks={};calls=[];removed=[];unsub=[];timers=[]
  class Variant:
   def __init__(self,signature,value):self.value=value
   def unpack(self):return self.value
  class Loop:
   def quit(self):pass
   def run(self):
    if owner_loss:
     callbacks['NameOwnerChanged'](None,None,None,None,None,Variant('',(m.SERVICE,':1.5','')),None)
    else:
     # Delayed historical HOLD/REMOVE must not prompt: current GetAll is idle.
     for needed in (True,False):
      callbacks['PropertiesChanged'](None,None,None,None,None,Variant('',(m.INTERFACE,{'finger-needed':needed,'finger-present':True},[])),None)
     timers[0]()
  class Connection:
   def connect(self,*args):pass
   def call_sync(self,dest,path,iface,method,params,reply,flags,timeout,cancel):
    calls.append((dest,method,flags))
    self.assert_flags=flags
    if method=='GetNameOwner':return Variant('',(':1.5',))
    if method=='GetDevices':return Variant('',(['/net/reactivated/Fprint/Device/0'],))
    if method=='GetAll':return Variant('',({'name':m.NAME,'finger-needed':False,'finger-present':False},))
    raise AssertionError('Unexpected method '+method)
   def signal_subscribe(self,sender,iface,signal,path,arg,flags,cb):callbacks[signal]=cb;return len(callbacks)
   def signal_unsubscribe(self,ident):unsub.append(ident)
   def flush_sync(self,*args):pass
  connection=Connection()
  glib=types.SimpleNamespace(MainLoop=Loop,Variant=Variant,VariantType=types.SimpleNamespace(new=lambda s:s),Error=RuntimeError,
   MainContext=types.SimpleNamespace(default=lambda:types.SimpleNamespace(pending=lambda:False)),
   timeout_add_seconds=lambda secs,cb:(timers.append(cb) or 9),source_remove=lambda n:removed.append(n))
  gio=types.SimpleNamespace(bus_get_sync=lambda *args:connection,BusType=types.SimpleNamespace(SYSTEM=1),DBusCallFlags=types.SimpleNamespace(NO_AUTO_START=2),DBusSignalFlags=types.SimpleNamespace(NONE=0))
  gi=types.ModuleType('gi');gi.require_version=lambda *args:None
  repo=types.ModuleType('gi.repository');repo.Gio=gio;repo.GLib=glib
  output=io.StringIO()
  with patch.dict(sys.modules,{'gi':gi,'gi.repository':repo}),contextlib.redirect_stdout(output):
   result=m.main(['--timeout','1'])
  self.assertIn('Monitor connected. KEEP READER EMPTY.',output.getvalue())
  self.assertIn('fprintd-verify',output.getvalue());self.assertIn('wait for PLACE',output.getvalue())
  self.assertNotIn('reader verified',output.getvalue());self.assertNotIn('REMOVE your',output.getvalue())
  self.assertEqual(result,1 if owner_loss else 0)
  self.assertEqual(removed,[9] if owner_loss else [])
  self.assertEqual(len(unsub),2)
  self.assertTrue(all(flags==2 for dest,method,flags in calls))
  self.assertTrue(all(dest==':1.5' for dest,method,flags in calls if method!='GetNameOwner'))
  self.assertLessEqual({method for dest,method,flags in calls},{'GetNameOwner','GetDevices','GetAll'})

if __name__=='__main__':unittest.main()
