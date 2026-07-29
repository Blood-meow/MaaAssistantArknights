#include "DepotRecognitionTask.h"

#include <future>

#include <meojson/json.hpp>

#include "Config/GeneralConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/DepotImageAnalyzer.h"

bool asst::DepotRecognitionTask::_run()
{
    LogTraceFunction;

    bool ret = swipe_and_analyze();

    // 材料页扫完后，切到「全部」标签页识别基础物品（含寻访凭证）
    ret &= analyze_basic_items();

    callback_analyze_result(true);

    return ret;
}

bool asst::DepotRecognitionTask::analyze_basic_items()
{
    LogTraceFunction;

    // 识别并点击「全部」标签页（此时在材料页，「全部」为白色可选状态）
    Matcher all_tab_matcher(ctrler()->get_image());
    all_tab_matcher.set_task_info("DepotAllTab");
    auto all_tab_result = all_tab_matcher.analyze();
    if (!all_tab_result) {
        Log.error(__FUNCTION__, "failed to match DepotAllTab");
        return false;
    }
    ctrler()->click(all_tab_result->rect);
    sleep(500);

    // 材料页扫描结束时列表停在最右侧。切换标签不会重置横向位置，
    // 先把「全部」页拉回起点，否则只能识别尾部当前屏。
    static constexpr int ResetSwipeTimes = 12;
    for (int i = 0; i < ResetSwipeTimes; ++i) {
        swipe_basic_items(false);
    }

    // 7003/7004 的 sortId 在 400xx 段，和首屏 100xx 基础货币相隔很远。
    // 必须分页扫描；某一页没有目标物品也不能提前终止。
    static constexpr int MaxBasicItemPages = 16;
    static const std::vector<std::string> BasicItemIds = {
        "4002", "4003", "4001", "3003", "4006", "7003", "7004"
    };
    bool recognized_any = false;
    for (int page = 0; page < MaxBasicItemPages; ++page) {
        DepotImageAnalyzer analyzer(ctrler()->get_image());
        analyzer.set_item_ids(BasicItemIds);
        analyzer.set_is_basic(true);
        if (analyzer.analyze()) {
            recognized_any = true;
            for (const auto& [item_id, item_info] : analyzer.get_result()) {
                m_all_items.insert_or_assign(item_id, item_info);
            }
            callback_analyze_result(false);
        }

        if (m_all_items.contains("7003") && m_all_items.contains("7004")) {
            break;
        }
        swipe_basic_items(true);
    }

    DepotImageAnalyzer::clear_cached_templates();
    return recognized_any;
}

bool asst::DepotRecognitionTask::swipe_and_analyze()
{
    LogTraceFunction;
    m_all_items.clear();

    size_t pre_pos = 0ULL;
    while (true) {
        DepotImageAnalyzer analyzer(ctrler()->get_image());

        auto future = std::async(std::launch::async, [&]() { swipe(); });

        // 因为滑动不是完整的一页，有可能上一次识别过的物品，这次仍然在页面中
        // 所以这个 begin pos 不能设置
        // analyzer.set_match_begin_pos(pre_pos);
        if (!analyzer.analyze()) {
            break;
        }
        size_t cur_pos = analyzer.get_match_begin_pos();
        if (cur_pos == pre_pos || cur_pos == DepotImageAnalyzer::NPos) {
            break;
        }
        pre_pos = cur_pos;

        auto cur_result = analyzer.get_result();
        m_all_items.merge(std::move(cur_result));

        future.wait();
        callback_analyze_result(false);
    }
    DepotImageAnalyzer::clear_cached_templates();
    return !m_all_items.empty();
}

void asst::DepotRecognitionTask::callback_analyze_result(bool done)
{
    LogTraceFunction;

    json::value info = basic_info_with_what("DepotInfo");
    auto& details = info["details"];

    // 简化格式：只保留 done 和 data
    // data 为 {"itemId": count} 格式
    json::object data_obj;
    for (const auto& [item_id, item_info] : m_all_items) {
        data_obj.emplace(item_id, item_info.quantity);
    }

    details["done"] = done;
    details["data"] = data_obj.to_string();

    callback(AsstMsg::SubTaskExtraInfo, info);
}

void asst::DepotRecognitionTask::swipe()
{
    ProcessTask(*this, { "DepotSlowlySwipeToTheRight" }).run();
}

void asst::DepotRecognitionTask::swipe_basic_items(bool to_right)
{
    ProcessTask(*this, { to_right ? "DepotSlowlySwipeToTheRight" : "DepotSlowlySwipeToTheLeft" }).run();
}
