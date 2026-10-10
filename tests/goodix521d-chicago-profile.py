"""Pure synthetic ABI/profile regressions; no reader, DLL or private fixtures."""
import ctypes as C, os, unittest
class Profiles(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.lib=C.CDLL(os.environ['CHICAGO_TEST_LIBRARY']); cls.glib=C.CDLL('libglib-2.0.so.0'); cls.Frame=C.c_uint16*5120
  cls.lib.goodix_chicago_calibration_generate.argtypes=[C.POINTER(C.c_uint16)];cls.lib.goodix_chicago_calibration_generate.restype=C.c_void_p
  cls.lib.goodix_chicago_preprocessor_new.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_void_p];cls.lib.goodix_chicago_preprocessor_new.restype=C.c_void_p
  cls.lib.goodix_chicago_preprocessor_free.argtypes=[C.c_void_p];cls.glib.g_bytes_unref.argtypes=[C.c_void_p]
 def test_profile_constructor_available(self):
  self.assertTrue(hasattr(self.lib,'goodix_chicago_preprocessor_new_for_profile'),'explicit profile constructor required')
 def test_invalid_profile_is_rejected(self):
  self.assertTrue(hasattr(self.lib,'goodix_chicago_preprocessor_new_for_profile'),'explicit profile constructor required')
  fn=self.lib.goodix_chicago_preprocessor_new_for_profile;fn.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];fn.restype=C.c_void_p
  ref=self.Frame(*([2000]*5120));payload=self.lib.goodix_chicago_calibration_generate(ref)
  try:self.assertFalse(fn(payload,ref,99,None))
  finally:self.glib.g_bytes_unref(payload)
 def test_profile_state_cannot_cross_restore(self):
  fn=self.lib.goodix_chicago_preprocessor_new_for_profile;fn.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];fn.restype=C.c_void_p
  class Array(C.Structure):_fields_=[('data',C.POINTER(C.c_ubyte)),('len',C.c_uint)]
  self.glib.g_byte_array_new.restype=C.POINTER(Array);self.glib.g_byte_array_unref.argtypes=[C.POINTER(Array)]
  self.lib.goodix_chicago_preprocessor_save_state.argtypes=[C.c_void_p,C.POINTER(Array)]
  self.lib.goodix_chicago_preprocessor_load_state.argtypes=[C.c_void_p,C.POINTER(C.c_ubyte),C.c_size_t,C.c_void_p];self.lib.goodix_chicago_preprocessor_load_state.restype=C.c_int
  ref=self.Frame(*([2000]*5120));payload=self.lib.goodix_chicago_calibration_generate(ref)
  hs=fn(payload,ref,24,None);gf=fn(payload,ref,17,None);a=self.glib.g_byte_array_new();b=self.glib.g_byte_array_new()
  try:
   self.lib.goodix_chicago_preprocessor_save_state(hs,a);self.lib.goodix_chicago_preprocessor_save_state(gf,b)
   self.assertFalse(self.lib.goodix_chicago_preprocessor_load_state(gf,a.contents.data,a.contents.len,None),'legacy mode24 state must not load into mode17')
   self.assertFalse(self.lib.goodix_chicago_preprocessor_load_state(hs,b.contents.data,b.contents.len,None))
   self.assertTrue(self.lib.goodix_chicago_preprocessor_load_state(gf,b.contents.data,b.contents.len,None))
  finally:
   self.glib.g_byte_array_unref(a);self.glib.g_byte_array_unref(b);self.lib.goodix_chicago_preprocessor_free(hs);self.lib.goodix_chicago_preprocessor_free(gf);self.glib.g_bytes_unref(payload)
 def make_profile(self,profile,ref):
  fn=self.lib.goodix_chicago_preprocessor_new_for_profile;fn.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];fn.restype=C.c_void_p
  payload=self.lib.goodix_chicago_calibration_generate(ref)
  try:return fn(payload,ref,profile,None)
  finally:self.glib.g_bytes_unref(payload)
 def test_gf5298_replicates_all_edges_after_clamp(self):
  ref=self.Frame(*([2000]*5120));gf=self.make_profile(17,ref);hs=self.make_profile(24,ref)
  raw=self.Frame(*[(y*80+x)%4096 for y in range(64) for x in range(80)]);raw[81]=65535
  prep=self.lib.goodix_chicago_preprocessor_prepare_raw;prep.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.POINTER(C.c_uint16)]
  try:
   out=self.Frame();prep(gf,raw,out)
   for y in range(64):
    for x in range(80):self.assertEqual(out[y*80+x],min(raw[min(62,max(1,y))*80+min(78,max(1,x))],4095))
   prep(hs,raw,out);self.assertEqual(out[0],raw[0]);self.assertEqual(out[80],raw[80]);self.assertEqual(out[1],4095)
  finally:self.lib.goodix_chicago_preprocessor_free(gf);self.lib.goodix_chicago_preprocessor_free(hs)
 def test_gf5298_reference_gate_is_strict85percent(self):
  coords=[y*80+x for y in range(2,62) for x in range(2,78)]
  ref=self.Frame()
  for i in coords[:3876]:ref[i]=2000
  obj=self.make_profile(17,ref)
  if obj:self.lib.goodix_chicago_preprocessor_free(obj)
  self.assertFalse(obj,'exactly85percent must reject')
  ref[coords[3876]]=2000;obj=self.make_profile(17,ref)
  try:self.assertTrue(obj,'strictlyabove85percent must pass')
  finally:
   if obj:self.lib.goodix_chicago_preprocessor_free(obj)
 def test_profile_calibration_prepares_full_gf5298_border(self):
  self.assertTrue(hasattr(self.lib,'goodix_chicago_calibration_generate_for_profile'))
  fn=self.lib.goodix_chicago_calibration_generate_for_profile;fn.argtypes=[C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];fn.restype=C.c_void_p
  self.glib.g_bytes_get_data.argtypes=[C.c_void_p,C.POINTER(C.c_size_t)];self.glib.g_bytes_get_data.restype=C.c_void_p
  ref=self.Frame(*([2000]*5120));ref[0]=7;ref[80]=8;ref[81]=1234
  payload=fn(ref,17,None)
  try:
   size=C.c_size_t();data=C.string_at(self.glib.g_bytes_get_data(payload,C.byref(size)),size.value)
   self.assertEqual(int.from_bytes(data[0x9928:0x992a],'little'),1234+7095)
   self.assertEqual(int.from_bytes(data[0x9928+160:0x992a+160],'little'),1234+7095)
  finally:self.glib.g_bytes_unref(payload)
 def test_gf5298_capture_gate_is_strict25percent(self):
  ref=self.Frame(*([2000]*5120));coords=[y*80+x for y in range(2,62) for x in range(2,78)]
  process=self.lib.goodix_chicago_preprocessor_process_context;process.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_int,C.c_int,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p];process.restype=C.c_uint
  raw=self.Frame();out=(C.c_ubyte*5120)();q=C.c_ubyte();coverage=C.c_ubyte();context=(C.c_ubyte*6)()
  for i in coords[:1140]:raw[i]=1500
  gf=self.make_profile(17,ref)
  try:self.assertEqual(process(gf,raw,0,0,out,C.byref(q),C.byref(coverage),context),0x29aa)
  finally:self.lib.goodix_chicago_preprocessor_free(gf)
  raw[coords[1140]]=1500;gf=self.make_profile(17,ref)
  try:self.assertNotEqual(process(gf,raw,0,0,out,C.byref(q),C.byref(coverage),context),0x29aa)
  finally:self.lib.goodix_chicago_preprocessor_free(gf)
 def test_gf5298_uniform_mask_and_saturation(self):
  self.assertTrue(hasattr(self.lib,'goodix_gf5298_build_mask'))
  fn=self.lib.goodix_gf5298_build_mask;fn.argtypes=[C.POINTER(C.c_uint16),C.POINTER(C.c_uint16),C.c_void_p];fn.restype=C.c_uint16
  current=self.Frame(*([1400]*5120));offset=self.Frame(*([9095]*5120));mask=(C.c_ubyte*5120)()
  self.assertEqual(fn(current,offset,mask),7595);self.assertEqual(sum(v!=0 for v in mask),5120)
  current[80*20+20]=3800;fn(current,offset,mask);self.assertEqual(mask[80*20+20],0)
 def test_gf5298_mask_fill_keeps_saturated_holes_disabled(self):
  self.assertTrue(hasattr(self.lib,'goodix_gf5298_fill_mask'))
  fn=self.lib.goodix_gf5298_fill_mask;fn.argtypes=[C.POINTER(C.c_uint16),C.c_void_p]
  current=self.Frame(*([1400]*5120));mask=(C.c_ubyte*5120)();mask[10*80+10]=mask[10*80+20]=255;current[10*80+15]=3800
  fn(current,mask)
  for x in range(80):self.assertEqual(bool(mask[10*80+x]),10<=x<=20 and x!=15)
 def test_gf5298_signed_signal_retains_negative_background_difference(self):
  self.assertTrue(hasattr(self.lib,'goodix_gf5298_corrected_signal'))
  fn=self.lib.goodix_gf5298_corrected_signal;fn.argtypes=[C.POINTER(C.c_uint16),C.POINTER(C.c_uint16),C.POINTER(C.c_uint16)]
  current=self.Frame(*([1600]*5120));offset=self.Frame(*([8095]*5120));out=self.Frame();fn(current,offset,out)
  self.assertEqual(set(out),{7200})
  current[0]=4095;offset[0]=7095;fn(current,offset,out);self.assertEqual(out[0],0)
 def test_gf5298_rebase_rejects_bad_reference_before_mutation(self):
  ref=self.Frame(*([2000]*5120));bad=self.Frame(*([4095]*5120));gf=self.make_profile(17,ref)
  payload=self.lib.goodix_chicago_calibration_generate(bad)
  fn=self.lib.goodix_chicago_preprocessor_rebase;fn.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_uint16),C.c_void_p];fn.restype=C.c_int
  get=self.lib.goodix_chicago_preprocessor_get_image_base;get.argtypes=[C.c_void_p];get.restype=C.POINTER(C.c_uint16)
  try:
   self.assertFalse(fn(gf,payload,bad,None));self.assertEqual(get(gf)[0],2000)
  finally:self.lib.goodix_chicago_preprocessor_free(gf);self.glib.g_bytes_unref(payload)
 def test_gf5298_local_gain_uses_separable_median(self):
  self.assertTrue(hasattr(self.lib,'goodix_gf5298_local_gain'))
  fn=self.lib.goodix_gf5298_local_gain;fn.argtypes=[C.POINTER(C.c_uint16),C.POINTER(C.c_uint16)]
  signal=self.Frame(*([3000]*5120));gain=self.Frame(*([8192]*5120));pattern=[[0,10000,10000],[10000,1000,10000],[0,0,0]]
  for y in range(3):
   for x in range(3):signal[(20+y)*80+20+x]=pattern[y][x]
  fn(signal,gain);self.assertEqual(gain[21*80+21],819)
  self.assertEqual(gain[0],8192)
 def test_gf5298_normalization_keeps_signed_signal_when_adaptation_gated(self):
  ref=self.Frame(*([1000]*5120));raw=self.Frame(*[1600 if y%4==0 else 2600 for y in range(64) for x in range(80)])
  gen=self.lib.goodix_chicago_calibration_generate_for_profile;gen.argtypes=[C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];gen.restype=C.c_void_p
  fn=self.lib.goodix_chicago_preprocessor_new_for_profile;fn.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_uint,C.c_void_p];fn.restype=C.c_void_p
  payload=gen(ref,17,None);gf=fn(payload,ref,17,None);out=self.Frame();aux=self.Frame()
  normalize=self.lib.goodix_test_normalize;normalize.restype=None;normalize.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.POINTER(C.c_uint16),C.c_int,C.POINTER(C.c_uint16),C.POINTER(C.c_uint16)]
  try:
   normalize(gf,raw,ref,0,out,aux)
   self.assertEqual(list(out),[3*(4000-v) for v in raw]);self.assertEqual(list(aux),list(out))
  finally:self.lib.goodix_chicago_preprocessor_free(gf);self.glib.g_bytes_unref(payload)
 def test_gf5298_alternate_selector_boundaries(self):
  self.assertTrue(hasattr(self.lib,'goodix_gf5298_select_alternate'))
  fn=self.lib.goodix_gf5298_select_alternate;fn.argtypes=[C.c_int,C.c_int,C.c_int];fn.restype=C.c_int
  for p,a,c,expected in [(799,0,0,False),(800,639,234,True),(800,640,234,False),(800,639,235,False),(1800,639,255,True),(3600,1199,255,True),(4500,1999,255,True),(4500,2000,0,False),(5400,99999,255,True)]:
   self.assertEqual(bool(fn(p,a,c)),expected,(p,a,c))
 def test_gf5298_mask_midpoint_rounding_crosses4095(self):
  fn=self.lib.goodix_gf5298_build_mask;fn.argtypes=[C.POINTER(C.c_uint16),C.POINTER(C.c_uint16),C.c_void_p];fn.restype=C.c_uint16
  current=self.Frame(*[2000 if x<64 else 4095 for y in range(64) for x in range(80)]);offset=self.Frame(*([7095]*5120));mask=(C.c_ubyte*5120)()
  self.assertEqual(fn(current,offset,mask),3525)
if __name__=='__main__':unittest.main()
