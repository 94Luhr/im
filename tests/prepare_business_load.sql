-- IM服务端数据库业务压测数据。
-- 默认准备1000对相互为好友的独立账号，密码统一为 LoadTest123!。
-- 账号仅使用 load_s 和 load_r 前缀，不会修改普通用户数据。

USE imdb;

DROP PROCEDURE IF EXISTS prepare_im_load_accounts;

DELIMITER //
CREATE PROCEDURE prepare_im_load_accounts(IN pair_count INT)
BEGIN
  DECLARE pair_index INT DEFAULT 1;
  -- 显式使用与t_user.tel相同的字符集和排序规则，避免MySQL 8
  -- 的连接默认排序规则utf8mb4_0900_ai_ci参与等值比较时发生冲突。
  DECLARE sender_tel VARCHAR(15)
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
  DECLARE receiver_tel VARCHAR(15)
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
  DECLARE sender_id INT;
  DECLARE receiver_id INT;

  WHILE pair_index <= pair_count DO
    SET sender_tel = CONCAT('load_s', LPAD(pair_index, 6, '0'));
    SET receiver_tel = CONCAT('load_r', LPAD(pair_index, 6, '0'));

    INSERT INTO t_user(name, tel, password, felling, iconid)
    VALUES(sender_tel, sender_tel, SHA2('LoadTest123!', 256), '数据库压测发送者', 8)
    ON DUPLICATE KEY UPDATE
      id=LAST_INSERT_ID(id),
      felling='数据库压测发送者';
    SET sender_id = LAST_INSERT_ID();

    INSERT INTO t_user(name, tel, password, felling, iconid)
    VALUES(receiver_tel, receiver_tel, SHA2('LoadTest123!', 256), '数据库压测接收者', 8)
    ON DUPLICATE KEY UPDATE
      id=LAST_INSERT_ID(id),
      felling='数据库压测接收者';
    SET receiver_id = LAST_INSERT_ID();

    INSERT IGNORE INTO t_friend(idA, idB) VALUES(sender_id, receiver_id);
    INSERT IGNORE INTO t_friend(idA, idB) VALUES(receiver_id, sender_id);

    SET pair_index = pair_index + 1;
  END WHILE;
END//
DELIMITER ;

CALL prepare_im_load_accounts(1000);
DROP PROCEDURE prepare_im_load_accounts;

-- 清理压测账号之间遗留的消息，确保离线投递数量可精确核对。
DELETE msg
FROM t_message AS msg
JOIN t_user AS sender ON sender.id=msg.sender_id
JOIN t_user AS receiver ON receiver.id=msg.receiver_id
WHERE sender.tel LIKE 'load_s%'
  AND receiver.tel LIKE 'load_r%';

SELECT COUNT(*) AS prepared_load_accounts
FROM t_user
WHERE tel LIKE 'load_s%' OR tel LIKE 'load_r%';
