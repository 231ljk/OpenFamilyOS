# adventure3d — 3D 场景闯关实践模块

把知识点包装成可闯的 3D 关卡。本模块提供**引擎接口约定 + 场景格式**，
渲染后端（OpenGL ES / Vulkan / WebGL / 自研）留给社区实现 ——
延续开轮版「接口 + 可替换」的一贯风格。

## 关卡如何与课程挂钩

课程 `course.json` 的 `adventure.scene` 字段指向 `scenes/*.json`；
学生在课堂上学完「认识分数」，进入「分数大厅」把掉落的分数块放对位置。
闯关结果（用时、错误、尝试次数）回写教育系统进度库，家长端可见。

## 场景 JSON 约定（v1）

```json
{
  "id": "fraction-hall",
  "title": "分数大厅",
  "world": { "kind": "hall", "skybox": "assets/sky-blue.png" },
  "objects": [ { "id": "slot-4-4", "mesh": "assets/pedestal.gltf",
                 "pos": [0, 0, -3], "accepts": ["3/4"] } ],
  "rules": { "goal": "把 6 个分数块放到正确的基座", "time_limit_s": 180,
             "win": "all_placed", "reward_stars": 3 },
  "teach": { "before": "每关开始播 60 秒知识点动画", "on_wrong": "语音提示" }
}
```

## 引擎必须实现的最小接口（伪代码）

```
load_scene(path)            → 解析场景 JSON
spawn(object) / remove(id)  → 场景对象生命周期
on_grab(obj, target)        → 交互回调，返回规则判定结果
frame(dt)                   → 渲染循环（后端自带）
report(result)              → 成绩回写 edu 进度库
```

社区若只想做**2D 版**：保持场景 JSON 与回调名不变，渲染层自由降级 ——
同一关卡数据可多端复用（电脑/平板全屏、电视大屏）。

## 目录

- `scenes/` 场景 JSON
- `engine.md`（本文件）接口约定
- 渲染后端示例实现欢迎 PR：建议从 `webgl`（复用平板浏览器）或
  `raylib`（最小说明）起步。
