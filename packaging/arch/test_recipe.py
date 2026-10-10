import pathlib,unittest,subprocess,tempfile,shutil
ROOT=pathlib.Path(__file__).resolve().parents[2]
class Packaging(unittest.TestCase):
 def test_metadata_and_pinned_checkout(self):
  src=ROOT/'packaging/arch/PKGBUILD'; self.assertTrue(src.is_file(),'PKGBUILD missing')
  with tempfile.TemporaryDirectory() as t:
   repo=pathlib.Path(t)/'checkout'; pkg=repo/'packaging/arch';pkg.mkdir(parents=True);shutil.copy(src,pkg/'PKGBUILD')
   def git(*args):return subprocess.check_output(['git','-C',str(repo),*args],text=True).strip()
   git('init','-q');git('add','.');git('-c','user.name=Test','-c','user.email=test@example.invalid','commit','-qm','test')
   sha=git('rev-parse','HEAD')
   script='source "$startdir/PKGBUILD"; printf "%s\\n" "${source[@]}" "${provides[@]}" "${conflicts[@]}" "$pkgname" "$pkgver"; declare -p license depends; arch-meson(){ printf "arch-meson %s\\n" "$*"; }; meson(){ printf "meson %s\\n" "$*"; }; build; package'
   r=subprocess.run(['bash','-euc',script],env={'PATH':'/usr/bin:/bin','startdir':str(pkg),'pkgdir':str(repo/'pkg'),'srcdir':str(repo/'src')},capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr);self.assertIn('#commit='+sha,r.stdout);self.assertIn('\nlibfprint-goodix-521d\n',r.stdout);self.assertIn('libfprint=1.94.100',r.stdout);self.assertIn('libfprint-2.so',r.stdout);self.assertIn('LGPL-2.1-or-later',r.stdout);self.assertIn('gcc-libs',r.stdout);self.assertIn('libfprint-git=1.94.100.r',r.stdout);self.assertIn('installed-tests=false',r.stdout);self.assertIn('meson test -C build --print-errorlogs',src.read_text());self.assertIn('--destdir',r.stdout)
   self.assertNotIn('merge_requests',r.stdout)
   r2=subprocess.run(['bash','-euc','source "$startdir/PKGBUILD"; _check_device_ids "2808:A97A 06cb:010B 27c6:521d"; ! _check_device_ids "2808:A97A 06cb:010B"'],env={'PATH':'/usr/bin:/bin','startdir':str(pkg)},capture_output=True,text=True)
   self.assertEqual(r2.returncode,0,r2.stderr);self.assertIn('27c6:521d missing',r2.stderr)
   recipe=src.read_text();self.assertIn('2808:a97a',recipe);self.assertIn('06cb:010b',recipe);self.assertIn('27c6:521d',recipe)
   clone=repo/'src/libfprint';clone.parent.mkdir(parents=True)
   subprocess.run(['git','clone','-q',str(repo),str(clone)],check=True)
   env={'PATH':'/usr/bin:/bin','startdir':str(pkg),'pkgdir':str(repo/'pkg'),'srcdir':str(repo/'src')}
   r=subprocess.run(['bash','-euc','source "$startdir/PKGBUILD"; prepare'],env=env,capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
   with (pkg/'PKGBUILD').open('a') as f:f.write('\n# uncommitted edit\n')
   r=subprocess.run(['bash','-euc','source "$startdir/PKGBUILD"; prepare'],env=env,capture_output=True,text=True)
   self.assertNotEqual(r.returncode,0);self.assertIn('Commit or stash',r.stderr)
if __name__=='__main__':unittest.main()
