-- IM服务端自动化集成测试数据。
-- 仅重置以itest_开头的四个专用账号之间的数据，不影响普通用户。

USE imdb;

INSERT INTO t_user(name, tel, password, felling, iconid)
VALUES('itest_sender', 'itest_sender', SHA2('Integration1!', 256), '集成测试发送者', 8)
ON DUPLICATE KEY UPDATE
  id=LAST_INSERT_ID(id), felling='集成测试发送者';
SET @sender_id=LAST_INSERT_ID();

INSERT INTO t_user(name, tel, password, felling, iconid)
VALUES('itest_receiver', 'itest_receiver', SHA2('Integration1!', 256), '集成测试接收者', 8)
ON DUPLICATE KEY UPDATE
  id=LAST_INSERT_ID(id), felling='集成测试接收者';
SET @receiver_id=LAST_INSERT_ID();

INSERT INTO t_user(name, tel, password, felling, iconid)
VALUES('itest_request', 'itest_request', SHA2('Integration1!', 256), '集成测试申请者', 8)
ON DUPLICATE KEY UPDATE
  id=LAST_INSERT_ID(id), felling='集成测试申请者';
SET @requester_id=LAST_INSERT_ID();

INSERT INTO t_user(name, tel, password, felling, iconid)
VALUES('itest_target', 'itest_target', SHA2('Integration1!', 256), '集成测试被申请者', 8)
ON DUPLICATE KEY UPDATE
  id=LAST_INSERT_ID(id), felling='集成测试被申请者';
SET @target_id=LAST_INSERT_ID();

-- 消息场景中的两个账号保持双向好友关系。
INSERT IGNORE INTO t_friend(idA, idB)
VALUES(@sender_id, @receiver_id), (@receiver_id, @sender_id);

-- 好友申请场景每次从非好友状态开始。
DELETE FROM t_friend
WHERE (idA=@requester_id AND idB=@target_id)
   OR (idA=@target_id AND idB=@requester_id);
DELETE FROM t_friend_request
WHERE requester_id=@requester_id AND target_id=@target_id;

-- 清除上一轮集成测试消息，使补发数量可以精确判断。
DELETE FROM t_message
WHERE sender_id=@sender_id AND receiver_id=@receiver_id
  AND content LIKE 'integration:%';

SELECT @sender_id AS sender_id,
       @receiver_id AS receiver_id,
       @requester_id AS requester_id,
       @target_id AS target_id;
