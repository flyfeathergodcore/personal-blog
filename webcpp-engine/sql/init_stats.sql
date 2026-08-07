-- site_stats 访问统计表：分钟级聚合落库（webcpp-engine 每 60s 写入一条）
-- 注意：本脚本用 CREATE TABLE IF NOT EXISTS，可安全重复执行；
-- 不可重跑 init.sql（其首行 DROP DATABASE IF EXISTS 会清空全部用户数据）。
SET NAMES utf8mb4;
CREATE TABLE IF NOT EXISTS site_stats (
  id INT AUTO_INCREMENT PRIMARY KEY,
  ts DATETIME NOT NULL,            -- 分钟级时间点
  req INT NOT NULL DEFAULT 0,      -- 请求数
  req_h1 INT NOT NULL DEFAULT 0, req_h2 INT NOT NULL DEFAULT 0,
  err INT NOT NULL DEFAULT 0,      -- 错误数
  bytes BIGINT NOT NULL DEFAULT 0, -- 传输字节
  p50 INT NOT NULL DEFAULT 0, p90 INT NOT NULL DEFAULT 0, p99 INT NOT NULL DEFAULT 0,
  act INT NOT NULL DEFAULT 0,      -- 平均活动连接（四舍五入存）
  act_max INT NOT NULL DEFAULT 0,  -- 峰值活动连接
  UNIQUE KEY uk_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
