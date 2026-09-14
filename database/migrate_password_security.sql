-- 扩展密码字段以保存带算法、迭代次数和随机盐的PBKDF2编码。
-- 该操作不会改写已有SHA-256密码；用户下次成功登录时服务端自动升级。

USE imdb;

ALTER TABLE t_user
  MODIFY COLUMN password VARCHAR(255) NOT NULL;
