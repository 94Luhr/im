CREATE DATABASE IF NOT EXISTS imdb
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE imdb;

CREATE TABLE IF NOT EXISTS t_user (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(15) NOT NULL,
  tel VARCHAR(15) NOT NULL,
  password VARCHAR(255) NOT NULL,
  felling VARCHAR(255) NOT NULL DEFAULT '',
  iconid INT NOT NULL DEFAULT 8,
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_name (name),
  UNIQUE KEY uk_user_tel (tel)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS t_friend (
  idA INT NOT NULL,
  idB INT NOT NULL,
  PRIMARY KEY (idA, idB),
  CONSTRAINT fk_friend_owner FOREIGN KEY (idA) REFERENCES t_user(id) ON DELETE CASCADE,
  CONSTRAINT fk_friend_target FOREIGN KEY (idB) REFERENCES t_user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS t_friend_request (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  requester_id INT NOT NULL,
  target_id INT NOT NULL,
  status TINYINT NOT NULL DEFAULT 0 COMMENT '0待处理，1已同意，2已拒绝',
  created_at BIGINT UNSIGNED NOT NULL,
  handled_at BIGINT UNSIGNED NULL,
  result_delivered_at BIGINT UNSIGNED NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_friend_request_pair (requester_id, target_id),
  KEY idx_friend_request_target_status (target_id, status, id),
  KEY idx_friend_result_delivery (requester_id, status, result_delivered_at, id),
  CONSTRAINT fk_friend_request_requester FOREIGN KEY (requester_id) REFERENCES t_user(id) ON DELETE CASCADE,
  CONSTRAINT fk_friend_request_target FOREIGN KEY (target_id) REFERENCES t_user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS t_message (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  sender_id INT NOT NULL,
  receiver_id INT NOT NULL,
  content TEXT NOT NULL,
  created_at BIGINT UNSIGNED NOT NULL,
  delivered_at BIGINT UNSIGNED NULL,
  PRIMARY KEY (id),
  KEY idx_message_receiver_delivery (receiver_id, delivered_at, id),
  KEY idx_message_conversation (sender_id, receiver_id, id),
  CONSTRAINT fk_message_sender FOREIGN KEY (sender_id) REFERENCES t_user(id) ON DELETE CASCADE,
  CONSTRAINT fk_message_receiver FOREIGN KEY (receiver_id) REFERENCES t_user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
