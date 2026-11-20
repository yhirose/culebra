# 2026-11-20 Scene の mat.depth_bias(slope, units)（小）

- 作業: flight-break（#74）の依頼で、材質に glPolygonOffset を足した。emit の DrawItem のループで depth_write / depth_test の隣に切り替え、パスの終わりで切る（影のパスは通らない）。rlgl が包んでいないので raylib の glad（libraylib.a の中、外部リンケージ）を `external/glad.h` で呼ぶ。docs（en・ja）の材質の表に1行、api_surface を生成し直し。3a93167e で着地。
- 見込みと実績: 見込みは小（C++ 1か所、devのビルド、テスト1本、docs）。実績は実装から着地まで約1.5時間で、うち CI の ci/ が約35分。dev は culebra_rt_scene.cc の1TUだけで数十秒。
- 確かめ方: 同じ平面の 100 km 四方の板2枚（片方は四半回転させて対角線を変え、地面を order で後に描く）を、真上30 m・真上40 km・1 kmの高さから40 km先・目の高さ2 mで300 m先の4視点、4倍の render_target と4倍のウィンドウで、カメラを少しずつ動かして数えた。macOS では (-1, -1) が真上40 kmでだけ地面を透かし、(-1, -4) で全視点 0。Linux（ci/ の linux-scene、Xvfb＋ソフトウェア GL）でも (-1, -4) で通った。テストは修正前（メソッドがない）、(0, 0)、(-1, -1)、および「(0, 0) で切らない」に壊した C++ で落ちることを確認。
- 後処理: just check、check-generated、check-quick-guide、doctest、scene_api_test.sh（サンドボックスの外。中ではウィンドウが開けない）。/simplify はコメント2件と .cul の書き方1件を採用。window backend なので ci/ を通してから着地（早送り、master の CI は回り直さない）。
- 気づき: Scene のウィンドウを開くテストはサンドボックスの中では `cannot open a window` で必ず落ちる（WindowServer への接続）。最初から外で回す。
