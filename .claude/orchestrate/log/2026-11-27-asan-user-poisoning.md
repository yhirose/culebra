# masterのASanが4回続けて赤だった件（2026-11-27）

- 規模: 小（原因の調査 → justfileの1行 → ci/でASanを1回）。
- 見積もりと実績: ASanのCIは75〜90分の見込みどおり（約80分）。調査から着地まで約2時間。
- 原因: 09b120b6の`split_into_parts`が使う`llvm::EquivalenceClasses`が中に`BumpPtrAllocator`を持ち、ASanでビルドしたculebraのファイルがpoisonをかける版を生成した。リンカがその版をASanなしのlibLLVMにも使わせ、`LLVMContext`の確保（`StructType::create`）がuse-after-poisonになった。e4b8e9c0（11-27 05:12）から4回赤。
- 直し: ASanのレーンの`ASAN_OPTIONS`に`allow_user_poisoning=0`（9d47ab20）。culebraは自分でpoisonをかけていない。
- 組み替え: macOSの静的LLVMでの最小の再現では起きず、手元でLinuxを組むより、落ち方が決定的なのでci/でASanを1回回すほうに切り替えた。
- 気づき:
  - `just land`のASanの警告は、4回分のrunのあいだ誰も拾っていなかった。着地のたびに出る警告は、自分の変更と関係なくても、その場で原因のcommitを確かめる。
  - ASanのrunの`--log-failed`は1,500行ほどで、`ERROR: AddressSanitizer`と`#[0-9]* 0x`をgrepすれば足りる。サンドボックスの外で書いたファイルは外の`TMPDIR`にある。
