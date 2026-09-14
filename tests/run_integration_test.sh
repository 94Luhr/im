#!/usr/bin/env bash
set -euo pipefail

# 无论从哪个目录执行，都先定位到项目根目录。
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
cd "${project_dir}"

db_host="${IM_DB_HOST:-127.0.0.1}"
db_user="${IM_DB_USER:-imserver}"
db_name="${IM_DB_NAME:-imdb}"

run_mysql() {
    if [[ -n "${IM_DB_PASSWORD:-}" ]]; then
        # 密码通过临时进程环境传递，不拼接到命令行和终端历史中。
        MYSQL_PWD="${IM_DB_PASSWORD}" mysql \
            --host="${db_host}" --user="${db_user}" "$@" "${db_name}"
    else
        mysql --host="${db_host}" --user="${db_user}" --password "$@" "${db_name}"
    fi
}

echo "[1/3] 准备专用集成测试账号和业务数据"
run_mysql < tests/prepare_integration_test.sql

echo "[2/3] 执行核心业务集成测试"
python3 tests/integration_test.py "$@"

echo "[3/3] 验证密码哈希已经自动升级且随机盐互不相同"
security_sql='SELECT COUNT(*),COUNT(DISTINCT password) FROM t_user WHERE tel IN ("itest_sender","itest_receiver","itest_request","itest_target") AND LEFT(password,14)="pbkdf2_sha256$";'
read -r upgraded_count unique_hash_count < <(
    run_mysql --batch --skip-column-names --execute "${security_sql}"
)
if [[ "${upgraded_count}" != "4" || "${unique_hash_count}" != "4" ]]; then
    echo "密码安全验证失败：升级账号数=${upgraded_count:-0}，唯一哈希数=${unique_hash_count:-0}" >&2
    exit 1
fi
echo "密码安全验证通过：4个账号均为PBKDF2，且使用4个不同的随机盐"
