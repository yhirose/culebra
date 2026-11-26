# 最適化済みの静的LLVMを既定にする: flight-break #143 の完了と #154 #156 への分割（2026-11-26）

- 規模: 大（別リポジトリ culebra-llvm、GitHub Actionsのビルド、culebra側の取得とpin、CI/リリースの検査、Windows/Linuxの別マシンへの引き継ぎ）。担当は出さず、直列で進めた。
- 経緯: Homebrewへの報告をやめ（user判断）、自前ビルドに切り替え。culebra-llvmを公開リポジトリとして作り、`llvm-23.1.5-r1`（macOS arm64）を公開。culebraは`misc/prebuilt_llvm.env`のpinと`misc/prebuilt_llvm.sh`で取り、justfileの既定にした。`CULEBRA_SYSTEM_LLVM=1`で戻せる。macOSのCIとリリースは成果物がなければ落ちる。pinの検査は`check-generated`に入れた。着地は24714570（ci/ブランチの全ジョブ緑、rebaseなしの早送り）。
- 実績: `look.cul --jit`の翻訳 98秒（-O0）→約61秒（自前、-mcpu=apple-m1）。temps（#146）と合わせて108→61秒。ビルドの試行: 通常のRelease 約67秒、-mcpu付き約61秒、公式を変換した参考値 約52秒（配布不可）。ActionsのビルドはmacOS-15で約30分。
- 組み替え: (1) 公式リリースの流用は、ThinLTOビットコードと-fno-rttiで不可と分かり、自前ビルドに替えた。(2) 後から、Homebrewの静的LLVMを残す案をやめ、成果物のある環境では自前だけを使う形にした（`brew install culebra`はリリースtarballを配るので、ソースビルドはしない。この点を私が取り違えて、userに訂正された）。
- つまずき: macos-14ランナーが使えない（macos-15へ）。タグのビルドが、cmake引数の途中の`#`コメントで落ちた（コメントを引数の外へ、タグを打ち直した）。ランナーを48分で失い、ログなしで落ちた（再実行）。着地の順序で、他セッションの6コミットがrebaseで別ハッシュになり、localのmasterがoriginと分かれた（userの承認で、検証済みの木と同じ親へ`reset --hard`してpush）。検査スクリプトのBSD sedの`\|`が効かず、`sed -nE`へ。
- 守備範囲の外で回した後処理: なし。重いビルドはCI（ci/ブランチ）に任せた。
- 残り（別のIssueへ）: #154（`just look`の窓を30秒以内に。翻訳は平均0.96コアで、97%がLLVMのoptimize 24秒とcodegen 43秒。module分割の並列化が有力、未試作）、#156（携帯機の起動時間。端末で翻訳しない形）。Windows/Linuxは別マシンの成果物待ち（envに3行ずつ、Windowsは`configure_windows_release.sh`とci.ymlへ配線）。`terrain.cul`の約2倍は未計測。リリースノート: macOSのJIT翻訳が速くなった（`/release`の時に）。
- スキルへの気づき: 別リポジトリのビルド成果物に依存する変更は、「成果物がない環境」と「ある環境」の両方で、CIが何を見るかを先に決めておくと手戻りが減る（今回は途中で`REQUIRE`を足した）。
