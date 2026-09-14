-- 为已有数据库增加好友申请处理结果可靠通知字段和查询索引。
-- 脚本可重复执行，不会删除或覆盖现有申请记录。

USE imdb;

SET @column_exists = (
  SELECT COUNT(*)
  FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA=DATABASE()
    AND TABLE_NAME='t_friend_request'
    AND COLUMN_NAME='result_delivered_at'
);
SET @column_sql = IF(
  @column_exists=0,
  'ALTER TABLE t_friend_request ADD COLUMN result_delivered_at BIGINT UNSIGNED NULL AFTER handled_at',
  'SELECT 1'
);
PREPARE column_statement FROM @column_sql;
EXECUTE column_statement;
DEALLOCATE PREPARE column_statement;

SET @index_exists = (
  SELECT COUNT(*)
  FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA=DATABASE()
    AND TABLE_NAME='t_friend_request'
    AND INDEX_NAME='idx_friend_result_delivery'
);
SET @index_sql = IF(
  @index_exists=0,
  'ALTER TABLE t_friend_request ADD KEY idx_friend_result_delivery(requester_id,status,result_delivered_at,id)',
  'SELECT 1'
);
PREPARE index_statement FROM @index_sql;
EXECUTE index_statement;
DEALLOCATE PREPARE index_statement;
