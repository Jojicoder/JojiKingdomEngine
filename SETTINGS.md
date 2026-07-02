# JojiKingdomEngine — 全設定リファレンス

ファイル: `include/jke/core/Constants.hpp` をベースに各エンジンの定数も収録。

---

## ワールド設定

| 定数 | 値 | 説明 |
|---|---|---|
| `MAP_SIZE` | 257 | マップ幅・高さ (2^8+1、ダイヤモンドスクエア法) |
| `NUM_KINGDOMS` | 20 | 初期国家数 (起動オプションで変更可) |
| `NUM_RIVERS` | 12 | 河川数 |
| `LAKE_PROB` | 0.04 | 各タイルが湖になる確率 |

### 地形しきい値 (高度0.0〜1.0)

| 地形 | しきい値 |
|---|---|
| Ocean | < 0.20 |
| Coast | 0.20〜0.25 |
| Plain | 0.25〜0.45 |
| Forest | 0.45〜0.58 |
| Hill | 0.58〜0.70 |
| Mountain | > 0.70 |
| River source | > 0.72 |

### 地形移動コスト (BASE_MOVEMENT=4.0/ターン)

| 地形 | コスト |
|---|---|
| Plain | 1.0 |
| River | 1.3 |
| Coast | 1.5 |
| Forest | 1.8 |
| Hill | 2.0 |
| Mountain | 4.0 |
| Ocean / Lake | 通行不可 (999) |

### 地形肥沃度 (食料生産倍率)

| 地形 | 倍率 |
|---|---|
| River | 1.3 |
| Plain | 1.0 |
| Coast | 0.7 |
| Forest | 0.6 |
| Hill | 0.4 |

---

## 中立拠点

| 設定 | 値 |
|---|---|
| 目標配置数 | `NUM_KINGDOMS * 8 + 200` から既存都市数を引いた数 (20国の場合 ~200個) |
| 拠点間最小距離 | 8 タイル |
| 既存都市からの最小距離 | 9 タイル |
| スコアリング | ランダム 0〜10 + 地形ボーナス (河川+2、海岸+1.5、山岳+1.2、丘+0.8、森+0.6) |
| 視覚化 | クリーム色 `rgb(215,205,160)` 82%ブレンド |

---

## 経済

| 定数 | 値 | 説明 |
|---|---|---|
| `BASE_FOOD_PER_CITY` | 50.0 | 都市あたり基本食料/ターン |
| `BASE_GOLD_PER_CITY` | 30.0 | 都市あたり基本金/ターン |
| `ARMY_UPKEEP_PER_1K` | 5.0 | 兵士1000人あたりの維持費 (gold/ターン) |
| `FOOD_PER_1K_POP` | 1.0 | 人口1000人あたり食料消費 |
| `STARVATION_POP_LOSS` | 0.02 | 飢餓時の人口減少率/ターン |
| `STARVATION_MORALE` | 0.05 | 飢餓時の士気低下 |

### 初期資源

| 資源 | 初期値 |
|---|---|
| Food | 200 |
| Gold | 150 |
| Wood | 100 |
| Stone | 80 |
| Iron | 60 |

### 戦争疲弊 (warWeariness)

| しきい値 | 効果 |
|---|---|
| > 0.20 | 生産力・交易に疲弊補正開始 |
| > 0.25 | 軍維持費ペナルティ開始 |
| > 0.50 | 和平受諾しやすくなる |
| > 0.55 | 双方疲弊 → 強制和平交渉 |
| > 0.65 | 敗者が大きな領土譲渡 |
| > 0.70 | 属国化リスク |

---

## 安定性・反乱

| 定数 | 値 | 説明 |
|---|---|---|
| `REBELLION_THRESHOLD` | 0.30 | 安定性がこれ以下で反乱リスク |
| `CIVIL_WAR_THRESHOLD` | 0.15 | 内戦発生ライン |

---

## 戦闘

| 定数 | 値 | 説明 |
|---|---|---|
| `RANDOM_COMBAT_FACTOR` | 0.10 | 戦闘結果の乱数幅 ±10% |
| `RETREAT_MORALE_THRESHOLD` | 0.25 | 士気がこれ以下で撤退 |

### 兵種×地形戦闘倍率

|  | Ocean | Coast | Plain | Forest | Hill | Mountain | River | Lake |
|---|---|---|---|---|---|---|---|---|
| Militia | 0.0 | 0.8 | 1.0 | 0.9 | 0.8 | 0.6 | 0.9 | 0.0 |
| Infantry | 0.0 | 0.9 | 1.0 | 1.0 | 1.1 | 1.1 | 0.9 | 0.0 |
| Spearmen | 0.0 | 0.9 | 1.0 | 0.9 | 1.0 | 1.0 | 0.9 | 0.0 |
| Archers | 0.0 | 1.0 | 1.0 | 0.9 | 1.2 | 1.2 | 1.0 | 0.0 |
| Cavalry | 0.0 | 0.7 | 1.3 | 0.8 | 0.7 | 0.5 | 0.8 | 0.0 |
| Siege | 0.0 | 0.8 | 1.0 | 0.8 | 0.7 | 0.6 | 0.8 | 0.0 |

守備側は平地・海洋以外のすべての地形で +20%。

### 籠城・攻城

| 設定 | 値 |
|---|---|
| 要塞防御力 | `fortification × 85` |
| 人口防御 | `population × 0.003` |
| 基本駐屯力 | 60 |
| 攻城進捗率計算 | `(atkStr/defStr - 0.25) / (1 + fortification × 4.5)` |
| Aggressive 攻城ボーナス | × 1.25 |
| 籠城側食料枯渇ボーナス | × 2.0 (food < 20%) |
| 秋(Autumn) | × 1.65 (攻城好機) |
| 春(Spring) | × 0.90 |
| 冬(Winter) | × 0.40 |

### 軍訓練初期値 (personality別)

| 個性 | training |
|---|---|
| Aggressive | 0.95 |
| Opportunistic | 0.90 |
| Expansionist | 0.85 |
| Defensive | 0.82 |
| Economic | 0.65 |
| Diplomatic | 0.48 |

初期 equipment = 0.75、morale = 1.0、experience = 0.0

---

## 軍役割 (ArmyRole)

| 役割 | 色 (UI) | 動作 |
|---|---|---|
| Reserve | グレー | 待機 |
| Defense | 青紫 | 国境防衛 |
| Attack | オレンジ赤 | 敵都市攻撃 |
| Siege | 赤 | 包囲攻撃 |
| Vanguard | オレンジ | 最前線・最近敵都市を粘り強く追う |
| Flanker | 黄 | 副目標・別方向から侵攻 |
| Garrison | 水色 | 首都に常駐固定 |
| SupplyGuard | 緑 | 橋・補給拠点・港を巡回哨戒 |

---

## 海軍

| 設定 | 値 |
|---|---|
| 建造コスト | Gold 200 + Wood 150 |
| 維持費 | Gold 4/ターン |
| 移動クールダウン | 2ターン |
| 1回の移動ステップ数 | 最大 4 (BFS沿岸) |
| 乗船可能距離 (Chebyshev) | ≤ 8 タイル |
| 上陸判定距離 (Chebyshev) | ≤ 3 タイル |
| 海戦発生距離 (Chebyshev) | ≤ 1 タイル |
| 海戦ダメージ | 150 ± 50/交戦 |
| 初期hull | 1000 |
| 港喪失 → 艦隊消滅 | あり |
| 平和時 | 自国港間を巡回哨戒 |
| 経路探索 | BFS沿岸 (最大1500ノード) |

---

## 季節

| 定数 | 値 |
|---|---|
| `TURNS_PER_SEASON` | 20 |
| 1年 | 80ターン |
| 順序 | Spring → Summer → Autumn → Winter |

---

## バンディット

| 定数 | 値 |
|---|---|
| `BANDIT_SPAWN_INTERVAL` | 35ターン |
| `MAX_BANDIT_GROUPS` | 8 |
| 略奪Gold | 30 |
| 略奪Food | 20 |

---

## 技術

| 定数 | 値 |
|---|---|
| `MAX_TECH_LEVEL` | 5 |

---

## 国家ステータス上限

すべて `0.0〜1.0`：stability / morale / legitimacy / aggression / warWeariness

---

## 都市タイプ

| タイプ | 特徴 |
|---|---|
| Generic (Town) | 標準 |
| Port | 沿岸・gold収入・海軍拠点 |
| Fortress | 丘/山岳・要塞化高・攻略困難 |
| Agricultural (Farmland) | 河川/平地・food+人口 |
| Mining (Mine) | iron+stone |
| TradeHub | 金+安定性 |

## 建物タイプ

Farm / LumberMill / Quarry / IronMine / Market / Barracks / Walls / Fortress / Temple / Library / Aqueduct / Granary / Workshop

---

## 外交

| しきい値 | 効果 |
|---|---|
| `turnsAtWar > 25` | 和平受諾確率上昇 |
| `warWeariness > 0.50` | 和平受諾 |
| `lastWarDeclaredTurn + cooldown` | 再宣戦クールダウン |

### 戦略プラン (StrategyPlan)

HoldAndRecover / TurtleDefense / CapitalRush / BorderExpansion / OpportunisticRaid / RevengeWar / AntiHegemonWar / TotalConquest

### 戦争目標 (WarGoalType)

None / BorderCities / SupplyHub / Capital / Punitive / DefensiveHold / AntiHegemon / TotalConquest

---

## 国家個性 (KingdomPersonality)

| 個性 | 特徴 |
|---|---|
| Aggressive | 戦争好き・攻城+25%・高訓練 |
| Defensive | 守備重視・境界維持 |
| Economic | 経済優先・軍は傭兵頼り |
| Diplomatic | 同盟・条約重視・戦争回避 |
| Opportunistic | 好機主義・文化同化2× |
| Expansionist | 国境拡張・遠征文化 |

## 国家専門化 (KingdomSpecialization)

Military / Economy / Agriculture / Technology / Trade / Defense

---

*このファイルはコードから手動で抽出。定数を変更した場合は合わせて更新すること。*
