#include <iostream>
#include <string>
#include <http/httplib.h>
#include <http/json.hpp>
#include <http/wspp.h>

using json = nlohmann::json;

// 1. 获取 SharedJSContext 的 WebSocket 调试地址
std::string GetWebSocketUrl() {
    httplib::Client cli("127.0.0.1", 8080);
    auto res = cli.Get("/json");

    if (!res || res->status != 200) {
        return "";
    }

    try {
        auto targets = json::parse(res->body);
        for (auto& target : targets) {
            if (target.contains("title") && target["title"].is_string() && target["title"] == "SharedJSContext") {
                if (target.contains("webSocketDebuggerUrl") && target["webSocketDebuggerUrl"].is_string()) {
                    return target["webSocketDebuggerUrl"].get<std::string>();
                }
            }
        }
    }
    catch (...) {}
    return "";
}

// 2. 构造 JavaScript 脚本 (单行纯文本返回)
std::string BuildJsExpression(const std::string& target_sid) {
    std::string js = R"JS(
        (function() {
            try {
                const targetSid = ")JS" + target_sid + R"JS(";
                const offset = 76561197960265728n;
                const acc = Number(BigInt(targetSid) - offset);
                
                if (typeof friendStore === 'undefined' || !friendStore.GetFriendState) {
                    return "ERR|friendStore object not found";
                }
                
                const state = friendStore.GetFriendState(acc);
                if (!state) {
                    return "ERR|GetFriendState returned null";
                }

                const connObj = state?.m_persona?.m_mapRichPresence?.data_?.get("connect");
                const conn = connObj?.value_;
                
                if (conn) {
                    return "OK|" + String(conn);
                } else {
                    return "NOCONN|No connect field currently";
                }
            } catch(e) {
                return "ERR|JS Exception";
            }
        })()
    )JS";
    return js;
}

int main(int argc, char* argv[]) {
    // 获取命令行传入的 SteamID，如果没有传入则为空
    std::string target_sid = (argc > 1) ? argv[1] : "";
    bool is_test_mode = target_sid.empty();

    // 获取 WebSocket 调试地址，如果获取失败，说明 Steam 没开或者没开调试端口
    std::string ws_url = GetWebSocketUrl();
    if (ws_url.empty()) {
        std::cout << "ConnectFail" << std::endl;
        return 1;
    }

    wspp::ws_client ws;
    bool got_valid_result = false;
    bool connect_aborted = false;

    ws.on_open([&]() {
        if (is_test_mode) {
            // 如果输入为空，只测试连接
            std::cout << "ConnectSuccess" << std::endl;
            got_valid_result = true;
            ws.close();
        }
        else {
            // 如果输入了 SteamID，发送 CDP 请求
            json cdp_msg = {
                {"id", 1},
                {"method", "Runtime.evaluate"},
                {"params", {
                    {"expression", BuildJsExpression(target_sid)},
                    {"returnByValue", true}
                }}
            };
            std::string cdp_msg_str = cdp_msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore);
            ws.send(cdp_msg_str);
        }
        });

    ws.on_message([&](wspp::message_view msg) {
        if (!msg.is_text() || got_valid_result) return;

        std::string raw_msg(msg.text());
        try {
            auto resp = json::parse(raw_msg);
            if (resp.contains("id") && resp["id"].is_number_integer() && resp["id"].get<int>() == 1) {
                if (resp.contains("result") && resp["result"].is_object()) {
                    auto& result_node = resp["result"];

                    if (result_node.contains("exceptionDetails")) {
                        // JS 报错，视为解析错误
                        std::cout << "NotFound" << std::endl;
                        got_valid_result = true;
                    }
                    else if (result_node.contains("result") && result_node["result"].is_object()) {
                        auto& inner_node = result_node["result"];

                        if (inner_node.contains("value") && inner_node["value"].is_string()) {
                            std::string js_return_str = inner_node["value"].get<std::string>();

                            size_t pos = js_return_str.find('|');
                            if (pos != std::string::npos) {
                                std::string status = js_return_str.substr(0, pos);
                                std::string content = js_return_str.substr(pos + 1);

                                if (status == "OK") {
                                    // 成功获取 JVP 内容，直接输出内容，不带任何前缀
                                    std::cout << content << std::endl;
                                }
                                else if (status == "NOCONN" || status == "ERR") {
                                    // 没获取到 JVP 字段，或者内部报错
                                    std::cout << "NotFound" << std::endl;
                                }
                            }
                            else {
                                // 格式完全不对，也视为没找到
                                std::cout << "NotFound" << std::endl;
                            }
                            got_valid_result = true;
                        }
                    }
                }
                ws.close();
            }
        }
        catch (...) {}
        });

    ws.on_close([&](wspp::close_event ev) {
        if (ev.reason == wspp::detail::close_reason::aborted) {
            connect_aborted = true;
        }
        });

    auto res = ws.connect(ws_url);
    if (res != wspp::detail::ws_connect_result::ok) {
        std::cout << "ConnectFail" << std::endl;
        return 1;
    }

    // 阻塞运行，直到主动 ws.close() 或异常断开
    ws.run();

    // 如果运行结束，但没有任何有效输出（比如意外断网断连），返回 ConnectFail
    if (!got_valid_result) {
        if (connect_aborted || true) {
            std::cout << "ConnectFail" << std::endl;
        }
    }

    return 0;
}