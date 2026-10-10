#include "ConnectPanel.h"

namespace ConnectPanel {

Buttons buttons(const State& s)
{
    const bool hasSel = (s.selectedRow >= 0 && s.selectedRow < s.bookCount);

    Buttons b;
    // 「保存」只看**输入框里有没有东西可存**，故意不看 hasSel。
    // 这条正是那个 bug 的修法：新建态下 selectedRow 是 -1，旧规则在这里判死，
    // 于是第一条联系人也存不进去。onContactSave() 本来就支持"没选中就 add()"，
    // 只有这个使能条件写错了。
    b.save = s.editorValid;
    // 「删除」只能删已经存下的那一条 —— 这条才是真的依赖选中。
    b.del = hasSel;
    // 「连接」也不看地址簿有没有东西：填了码就该能连。地址簿是"常用设备"，
    // 不是"连人的前置条件"。
    b.connect = s.targetFilled && !s.sessionActive;
    b.disconnect = s.sessionActive;
    return b;
}

bool isNewMode(const State& s)
{
    return !(s.selectedRow >= 0 && s.selectedRow < s.bookCount);
}

Source sourceFor(bool editorTargetFilled, bool hasSelection, bool selectionMatchesEditors)
{
    if (!editorTargetFilled)
        return Source::None;
    return (hasSelection && selectionMatchesEditors) ? Source::Saved : Source::Adhoc;
}

} // namespace ConnectPanel
