# Sceneの窓でもテキスト入力を止める（2026-11-24）

- 規模: 小。Sceneの窓でキーを押し続けるとmacOSのアクセント選択が出る（ゲーム側の報告）。Canvasは586e003bで直してあり、Sceneは`InitWindow`の後に止めていなかった。
- 直し方: 止める処理と入れ直す処理を`include/stdlib/os_window.h`（両方の窓が既に使う共有の場所）に`os_window::stop_text_input`／`start_text_input`として移し、説明もそこに1か所。Sceneのコンストラクタとcanvasの`ensure_window()`が呼ぶ。Sceneは打った文字を読まない（`GetCharPressed`も`SDL_StartTextInput`もない）ので入れ直しは要らない。
- 確かめ方: 一時的に`SDL_TextInputActive`を前後で出すビルドで、Sceneの窓を開くと前=1、後=0（executorと`--jit`）。消して作り直し、Canvas（`CULEBRA_CANVAS_HEADLESS=0`、`Canvas.typed`を呼ぶ）とSceneの窓を両エンジンで開閉、`tests/scene_api_test.sh`、`just check`、`just check-generated`。自動のテストは足していない（スクリプトから見える状態がなく、効き目は押し続けたキーをmacOSがどう扱うかで、CIの窓では見えない。Canvasの修正も手元の確認だった）。
- 後処理: /simplify（3人）で、入れ直す側も共有の場所へ、ヘッダの説明を短く、の2件を採った。
- 気づき: 最初の`tests/scene_api_test.sh`が`timeout 120`で打ち切られたが、`bash -x`で回し直すと通った。初回の遅さか、手元の窓の都合とみて追っていない。
