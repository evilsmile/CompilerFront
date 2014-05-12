#include "RegExpAutomata.h"

#include <limits.h>
#include <stdlib.h>
#include <assert.h>
#include <algorithm>
#include "LexException.h"
#include "RegExpTokenizer.h"
#include "RegExpSyntaxTree.h"
#include "RegExpSynTreeNode.h"

RegExpNFA::RegExpNFA(bool partial)
    :AutomatonBase(AutomatonType_NFA), stateIndex_(0)
    ,headState_(-1), tailState_(-1), support_partial_match_(partial)
    ,states_(), NFAStatTran_()
#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    ,unitMatchPair_()
#endif
{
}

RegExpNFA::~RegExpNFA()
{
}

int RegExpNFA::BuildNFA(RegExpSyntaxTree* tree)
{
    stateIndex_ = 0;
    headState_ = tailState_ = -1;
    states_.clear();
    NFAStatTran_.clear();
    recycleStates_.clear();

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    unitMatchPair_.clear();
#endif

    int leaf_node_num = tree->GetNodeNumber() * 2;
    states_.reserve(leaf_node_num);
    recycleStates_.reserve(leaf_node_num/2);
    NFAStatTran_.reserve(leaf_node_num);

    int num = BuildNFAImp(dynamic_cast<RegExpSynTreeNode*>(tree->GetSynTree()), start_, accept_);

    if (support_partial_match_ && headState_ == -1)
    {
        for (int i = 0; i < STATE_TRAN_MAX; ++i)
        {
            if (NFAStatTran_[start_][i].empty()) NFAStatTran_[start_][i].push_back(start_);
        }
    }

    if (support_partial_match_ && tailState_ == -1)
    {
        for (int i = 0; i < STATE_TRAN_MAX; ++i)
        {
            if (NFAStatTran_[accept_][i].empty()) NFAStatTran_[accept_][i].push_back(accept_);
        }
    }

    return num;
}

// merge s2 into s1
void RegExpNFA::MergeState(int, int)
{
}

int RegExpNFA::CreateState(StateType type)
{
    int new_st;
    if (recycleStates_.empty())
    {
        new_st = stateIndex_++;
        MachineState state(new_st, type);
        std::vector<std::vector<int> > tmps(STATE_TRAN_MAX + 1);
        states_.push_back(state);
        NFAStatTran_.push_back(tmps);
    }
    else
    {
        new_st = recycleStates_[recycleStates_.size() - 1];
        recycleStates_.pop_back();
        assert(new_st < NFAStatTran_.size());
        assert(states_[new_st].GetType() & State_None);
    }

    return new_st;
}

void RegExpNFA::ReleaseState(int st)
{
    std::vector<std::vector<int> > tmps(STATE_TRAN_MAX + 1);

    NFAStatTran_[st].swap(tmps);
    states_[st].SetType(State_None);
}

int RegExpNFA::BuildNFAImp(RegExpSynTreeNode* root, int& start, int& accept)
{
    if (!root) return 0;

    int num = 0;
    if (root->IsLeafNode())
    {
        RegExpSynTreeLeafNode* ln = dynamic_cast<RegExpSynTreeLeafNode*>(root);
        assert(ln);

        num = BuildStateForLeafNode(ln, start, accept);
    }
    else if (root->GetNodeType() == RegExpSynTreeNodeType_Star)
    {
        RegExpSynTreeStarNode* sn = dynamic_cast<RegExpSynTreeStarNode*>(root);
        assert(sn);

        num = BuildStateForStarNode(sn, start, accept);
    }
    else if (root->GetNodeType() == RegExpSynTreeNodeType_Or)
    {
        num = BuildStateForOrNode(root, start, accept);
    }
    else if (root->GetNodeType() == RegExpSynTreeNodeType_Concat)
    {
        num = BuildStateForCatNode(root, start, accept);
    }
    else
    {
        assert(0);
    }

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    if (root->IsUnit())
    {
        states_[start].SetStartUnit();
        states_[accept].SetEndUnit();
        unitMatchPair_[start].insert(accept);
    }
#endif

    return num;
}

int RegExpNFA::BuildStateForLeafNode(RegExpSynTreeLeafNode* ln, int& start, int& accept)
{
    start = CreateState(State_Start);
    accept = CreateState(State_Accept);

    std::string txt = ln->GetNodeText();
    RegExpSynTreeNodeLeafNodeType lt = ln->GetLeafNodeType();
    if (lt == RegExpSynTreeNodeLeafNodeType_Dot)
    {
        std::vector<int> to(1, accept);
        std::vector<std::vector<int> > char_to_state(STATE_TRAN_MAX, to);
        char_to_state[STATE_EPSILON].clear();
        NFAStatTran_[start].swap(char_to_state);
    }
    else if (lt == RegExpSynTreeNodeLeafNodeType_Head)
    {
        std::vector<int> acc(1, accept);
        NFAStatTran_[start][STATE_EPSILON] = acc; // epsilon transition
        states_[start].AppendType(State_Head);
        headState_ = start;
    }
    else if(lt == RegExpSynTreeNodeLeafNodeType_Tail)
    {
        std::vector<int> acc(1, accept);
        NFAStatTran_[start][STATE_EPSILON] = acc; // epsilon transition
        states_[accept].AppendType(State_Tail);
        tailState_ = accept;
    }
    else if (lt == RegExpSynTreeNodeLeafNodeType_Alt)
    {
        std::vector<int> acc(1, accept);
        bool is_negate = (txt[0] == '^');
        if (!is_negate)
        {
            for (size_t i = 0; i < txt.size(); ++i)
            {
                if (txt[i] == '-' && (i > 0 && i < txt.size() - 1))
                {
                    if (txt[i - 1] > txt[i + 1]) throw LexErrException("range values reversed in []:", ln->GetNodeText().c_str());

                    for (size_t j = txt[i - 1]; j <= txt[i + 1]; ++j)
                    {
                        NFAStatTran_[start][j] = acc;
                    }
                    i += 1;
                }
                else
                {
                    if (i < txt.size() - 2 && txt[i] == '\\' && txt[i + 1] == '-')
                    {
                        ++i;
                    }
                    NFAStatTran_[start][txt[i]] = acc;
                }
            }
        }
        else
        {
            unsigned short chosen[STATE_TRAN_MAX] = {0};
            for (size_t i = 1; i < txt.size(); ++i)
            {
                if (txt[i] == '-' && (i > 0 && i < txt.size() - 1))
                {
                    if (txt[i - 1] > txt[i + 1]) throw LexErrException("range values reversed in []:", ln->GetNodeText().c_str());

                    for (size_t j = txt[i - 1]; j <= txt[i + 1]; ++j)
                    {
                        chosen[j] = 1;
                    }

                    i += 1;
                }
                else
                {
                    if (i < txt.size() - 2 && txt[i] == '\\' && txt[i + 1] == '-')
                    {
                        ++i;
                    }

                    chosen[txt[i]] = 1;
                }
            }

            for (size_t i = 1; i < STATE_TRAN_MAX; ++i)
            {
                if (chosen[i]) continue;

                NFAStatTran_[start][i] = acc;
            }
        }
    }
    else if (lt == RegExpSynTreeNodeLeafNodeType_Esc && (txt[0] == 's' || txt[0] == 'w' || txt[0] == 'd'))
    {
        std::vector<int> acc(1, accept);
        if (txt[0] == 's')
        {
            NFAStatTran_[start][' '] = acc;
        }
        else if (txt[0] == 'w')
        {
            for(int i = 'a'; i <= 'z'; ++i)
            {
                NFAStatTran_[start][i] = acc;
                NFAStatTran_[start][i + 'A' - 'a']  = acc;
            }
        }
        else
        {
            for(int i = '0'; i <= '9'; ++i)
            {
                NFAStatTran_[start][i] = acc;
            }
        }
    }
    else if (lt == RegExpSynTreeNodeLeafNodeType_Ref)
    {
#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
        RegExpSynTreeRefNode* rt = dynamic_cast<RegExpSynTreeRefNode*>(ln);
        NFAStatTran_[start][STATE_TRAN_MAX].push_back(accept);
        NFAStatTran_[start][STATE_TRAN_MAX].push_back(rt->GetRef());
        states_[start].AppendType(State_Ref);
        // states_[accept].AppendType(State_Ref);
#else
        assert(0);
#endif
    }
    else
    {
        std::vector<int> acc(1, accept);
        NFAStatTran_[start][txt[0]] = acc;
    }

    return 2;
}

int RegExpNFA::BuildStateForOrNode(RegExpSynTreeNode* node, int& start, int& accept)
{
    int left_child_start, left_child_accept;
    int right_child_start, right_child_accept;

    RegExpSynTreeNode* lc = dynamic_cast<RegExpSynTreeNode*>(node->GetLeftChild());
    RegExpSynTreeNode* rc = dynamic_cast<RegExpSynTreeNode*>(node->GetRightChild());

    assert(lc);
    assert(rc);

    start = CreateState(State_Start);
    int left_child_state_num = BuildNFAImp(lc, left_child_start, left_child_accept);
    int right_child_state_num = BuildNFAImp(rc, right_child_start, right_child_accept);
    accept = CreateState(State_Accept);

    states_[left_child_start].SetNormType();
    states_[right_child_start].SetNormType();
    NFAStatTran_[start][STATE_EPSILON].push_back(left_child_start); // epsilon transition
    NFAStatTran_[start][STATE_EPSILON].push_back(right_child_start);

    states_[left_child_accept].SetNormType();
    states_[right_child_accept].SetNormType();
    NFAStatTran_[left_child_accept][STATE_EPSILON].push_back(accept);
    NFAStatTran_[right_child_accept][STATE_EPSILON].push_back(accept);

    return left_child_state_num + right_child_state_num + 2;
}

int RegExpNFA::BuildStateForCatNode(RegExpSynTreeNode* node, int& start, int& accept)
{
    int left_child_start, left_child_accept;
    int right_child_start, right_child_accept;

    RegExpSynTreeNode* lc = dynamic_cast<RegExpSynTreeNode*>(node->GetLeftChild());
    RegExpSynTreeNode* rc = dynamic_cast<RegExpSynTreeNode*>(node->GetRightChild());

    assert(lc);
    assert(rc);

    int left_child_state_num = BuildNFAImp(lc, left_child_start, left_child_accept);
    int right_child_state_num = BuildNFAImp(rc, right_child_start, right_child_accept);

    states_[left_child_accept].SetNormType();
    states_[right_child_start].SetNormType();
    NFAStatTran_[left_child_accept][STATE_EPSILON].push_back(right_child_start);

    start = left_child_start;
    accept = right_child_accept;

    return left_child_state_num + right_child_state_num - 1;
}

int RegExpNFA::BuildStateForStarNode(RegExpSynTreeStarNode* sn, int& start, int& accept)
{
    int child_start, child_accept;
    RegExpSynTreeNode* child = dynamic_cast<RegExpSynTreeNode*>(sn->GetLeftChild());
    if (!child) throw LexErrException("\'*\' should come after specific character", NULL);

    bool is_unit = child->IsUnit();
    if (sn->GetMinRepeat() == 0 && sn->GetMaxRepeat() == INT_MAX)
    {
        // (ab)*
        start = CreateState(State_Start);
        int child_states_num = BuildNFAImp(child, child_start, child_accept);
        accept = CreateState(State_Accept);

        NFAStatTran_[start][STATE_EPSILON].push_back(child_start);
        NFAStatTran_[start][STATE_EPSILON].push_back(accept);

        states_[child_start].SetNormType();
        states_[child_accept].SetNormType();

        NFAStatTran_[child_accept][STATE_EPSILON].push_back(child_start);
        NFAStatTran_[child_accept][STATE_EPSILON].push_back(accept);

        return child_states_num + 2;
    }
    else if (sn->GetMinRepeat() == 0 && sn->GetMaxRepeat() == 1)
    {
        // (ab)?
        start = CreateState(State_Start);
        int child_states_num = BuildNFAImp(child, child_start, child_accept);
        accept = CreateState(State_Accept);

        states_[child_start].SetNormType();
        states_[child_accept].SetNormType();

        NFAStatTran_[start][STATE_EPSILON].push_back(child_start);
        NFAStatTran_[start][STATE_EPSILON].push_back(accept);
        NFAStatTran_[child_accept][STATE_EPSILON].push_back(accept);

        return child_states_num + 2;
    }
    else if (sn->GetMaxRepeat() == INT_MAX)
    {
        // (ab){3, INT_MAX}
        // (ab)+
        int min = sn->GetMinRepeat();
        int child_states_num = BuildNFAImp(child, child_start, child_accept);

        int cs, ca;
        int ts = child_start;
        int ta = child_accept;

        child->SetUnit(false);
        for (int i = 0; i < min - 1; ++i)
        {
            BuildNFAImp(child, cs, ca);

            states_[ta].SetNormType();
            states_[cs].SetNormType();
            NFAStatTran_[ta][STATE_EPSILON].push_back(cs);

            ts = cs;
            ta = ca;
        }

        states_[ta].SetNormType();

        start = child_start;
        accept = CreateState(State_Accept);

        NFAStatTran_[ta][STATE_EPSILON].push_back(ts);
        NFAStatTran_[ta][STATE_EPSILON].push_back(accept);

        child->SetUnit(is_unit);

        return child_states_num * min + 1;
    }
    else
    {
        //(ab){2, 4}
        int min = sn->GetMinRepeat();
        int max = sn->GetMaxRepeat();

        accept = CreateState(State_Accept);

        int child_states_num = BuildNFAImp(child, child_start, child_accept);
        // copy child min times

        int cs, ca;
        int ts = child_start;
        int ta = child_accept;

        start = child_start;

        child->SetUnit(false);
        for (int i = 0; i < min - 1; ++i)
        {
            BuildNFAImp(child, cs, ca);

            states_[ta].SetNormType();
            states_[cs].SetNormType();
            NFAStatTran_[ta][STATE_EPSILON].push_back(cs);

            ts = cs;
            ta = ca;
        }

        for (int i = min - 1; i < max - 1; ++i)
        {
            BuildNFAImp(child, cs, ca);
            states_[ta].SetNormType();
            states_[cs].SetNormType();
            NFAStatTran_[ta][STATE_EPSILON].push_back(cs);

            NFAStatTran_[ta][STATE_EPSILON].push_back(accept);

            ts = cs;
            ta = ca;
        }

        NFAStatTran_[ta][STATE_EPSILON].push_back(accept);

        child->SetUnit(is_unit);
        return child_states_num * max + 1;
    }
}

int RegExpNFA::BuildMachine(SyntaxTreeBase* tree)
{
    RegExpSyntaxTree* reg_tree = dynamic_cast<RegExpSyntaxTree*>(tree);
    if (!reg_tree) return 0;

    return BuildNFA(reg_tree);
}

int RegExpNFA::AddStateWithEpsilon(int st, std::vector<char>& isOn, std::vector<int>& to) const
{
    isOn[st] = 1;
    to.push_back(st);
    for (size_t i = 0; i < NFAStatTran_[st][STATE_EPSILON].size(); ++i)
    {
        int epsilon = NFAStatTran_[st][STATE_EPSILON][i];
        if (isOn[epsilon]) continue;

        AddStateWithEpsilon(epsilon, isOn, to);
    }

    return  to.size();
}

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
// if closure of state st has transition on input ch then return true;
// otherwise return false
bool RegExpNFA::IfStateClosureHasTrans(int st, std::vector<char>& isCheck, char ch) const
{
    const std::vector<int>& vc = NFAStatTran_[st][STATE_EPSILON];
    if (!NFAStatTran_[st][ch].empty()) return true;

    isCheck[st] = 1;
    for (size_t i = 0; i < vc.size(); ++i)
    {
        st = vc[i];
        if (isCheck[st]) continue;

        if (IfStateClosureHasTrans(st, isCheck, ch)) return true;
    }

    return false;
}

int RegExpNFA::SaveCaptureGroup(const std::map<int, const char*>& unitStart,
        int endState, const char* endTxt, std::vector<UnitInfo>& groupCapture_)
{
    int co = 0;
    std::map<int, const char*>::const_iterator it = unitStart.begin();
    for (; it != unitStart.end(); ++it)
    {
        int st = it->first;
        const char* txtStart = it->second;

        if (unitMatchPair_[st].find(endState) != unitMatchPair_[st].end())
        {
            co++;
            groupCapture_.push_back(UnitInfo(st, endState, txtStart, endTxt));
        }
    }

    return co;
}

void RegExpNFA::ConstructReferenceState(int st, int to, const char* ps, const char* pe)
{
    int new_st;

    states_[st].ClearType(State_Ref);
    while (ps <= pe)
    {
        new_st = CreateState(State_Norm);
        NFAStatTran_[st][*ps].push_back(new_st);
        st = new_st;
        ++ps;
    }

    NFAStatTran_[st][STATE_EPSILON].push_back(to);
}

void RegExpNFA::RestoreRefStates(int st, int to, const char* ps, const char* pe)
{
    assert(NFAStatTran_[st][*ps].size() == 1);
    int next_st = NFAStatTran_[st][*ps][0];
    NFAStatTran_[st][*ps].clear();
    st = next_st;

    ++ps;
    while (ps <= pe)
    {
        std::vector<int>& vc = NFAStatTran_[st][*ps];
        assert(vc.size() == 1);
        next_st = vc[0];
        ReleaseState(st);
        st = next_st;
        ++ps;
    }

    std::vector<int>& vc = NFAStatTran_[st][STATE_EPSILON];
    assert(vc.size() == 1);
    assert(vc[0] == to);
    ReleaseState(st);
}

#endif

/*
  unit matching: (e((a)|(b)ef), ((a|b)|(a|c)), (a(b))
*/
bool RegExpNFA::RunMachine(const char* ps, const char* pe)
{
#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    groupCapture_.clear();
#endif
    return RunNFA(start_, accept_, ps, pe);
}

bool RegExpNFA::RunNFA(int start, int accept, const char* ps, const char* pe)
{
    char ch;
    const char* in = ps;

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    std::vector<int> refStates;
    std::vector<int> curUnitEndStack;
    std::vector<int> curUnitStartStack;
    std::map<int, const char*> curUnitSelectedStack;

    refStates.reserve(states_.size());
    curUnitEndStack.reserve(states_.size());
    curUnitStartStack.reserve(states_.size());
    groupCapture_.reserve(states_.size());
#endif

    std::vector<int> curStat;
    std::vector<int> toStat;
    std::vector<char> alreadyOn(states_.size(), 0);

    curStat.reserve(states_.size());
    toStat.reserve(states_.size());

    AddStateWithEpsilon(start, alreadyOn, curStat);
    for (size_t i = 0; i < curStat.size(); ++i)
    {
        alreadyOn[curStat[i]] = false;
#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
        if (states_[curStat[i]].UnitStart()) curUnitStartStack.push_back(curStat[i]);
        if (states_[curStat[i]].UnitEnd()) curUnitEndStack.push_back(curStat[i]);
#endif
    }

    while (in <= pe && !curStat.empty())
    {
        ch = *in++;

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
        for (size_t j = 0; j < curUnitStartStack.size(); ++j)
        {
            int st = curUnitStartStack[j];

            std::vector<char> isCheck(states_.size(), 0);
            if (IfStateClosureHasTrans(st, isCheck, ch))
            {
                curUnitSelectedStack[st] = in - 1;
                continue;
            }
        }
        curUnitStartStack.clear();

        if (!curUnitEndStack.empty())
        {
            for (size_t i = 0; i < curStat.size(); ++i)
            {
                int st = curStat[i];
                const std::vector<int>& vc = NFAStatTran_[st][ch];
                if (vc.empty()) continue;

                for (size_t j = 0; j < vc.size(); ++j)
                {
                    if (alreadyOn[vc[j]]) continue;

                    AddStateWithEpsilon(vc[j], alreadyOn, toStat);
                }
            }

            for (size_t j = 0; j < curUnitEndStack.size(); ++j)
            {
                int st = curUnitEndStack[j];

                if (std::find(toStat.begin(), toStat.end(), st) == toStat.end())
                {
                    SaveCaptureGroup(curUnitSelectedStack, st, in - 2, groupCapture_);
                }
            }

            curUnitEndStack.clear();
        }
#endif

        for (size_t i = 0; i < curStat.size(); ++i)
        {
            int st = curStat[i];
            const std::vector<int>* vc = &(NFAStatTran_[st][ch]);

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
            if (states_[st].GetType() & State_Ref)
            {
                // reference state
                assert(NFAStatTran_[st][STATE_TRAN_MAX].size() == 2);

                int to   = NFAStatTran_[st][STATE_TRAN_MAX][0];
                int unit = NFAStatTran_[st][STATE_TRAN_MAX][1];

                assert(unit < groupCapture_.size());

                ConstructReferenceState(st, to,
                        groupCapture_[unit].txtStart_, groupCapture_[unit].txtEnd_);

                refStates.push_back(st);
                alreadyOn.resize(states_.size(), 0);
                vc = &(NFAStatTran_[st][ch]);
            }

#endif
            if (vc->empty()) continue;

            for (size_t j = 0; j < vc->size(); ++j)
            {
                if (alreadyOn[(*vc)[j]]) continue;

                AddStateWithEpsilon((*vc)[j], alreadyOn, toStat);
            }
        }

        curStat.swap(toStat);
        toStat.clear();

        for (size_t i = 0; i < curStat.size(); ++i)
        {
            alreadyOn[curStat[i]] = false;
#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
            if (states_[curStat[i]].UnitStart()
                    && curUnitSelectedStack.find(curStat[i]) == curUnitSelectedStack.end())
            {
                curUnitStartStack.push_back(curStat[i]);
            }

            if (states_[curStat[i]].UnitEnd()) curUnitEndStack.push_back(curStat[i]);
#endif
        }
    }

    for (size_t j = 0; j < curStat.size(); ++j)
    {
        if (alreadyOn[curStat[j]]) continue;

        AddStateWithEpsilon(curStat[j], alreadyOn, toStat);
    }
    curStat.swap(toStat);

#ifdef SUPPORT_REG_EXP_BACK_REFERENCE
    if (!curUnitEndStack.empty())
    {
        for (size_t j = 0; j < curUnitEndStack.size(); ++j)
        {
            int st = curUnitEndStack[j];
            SaveCaptureGroup(curUnitSelectedStack, st, pe, groupCapture_);
        }
    }

    // restore ref states
    for (size_t i = 0; i < refStates.size(); ++i)
    {
        int st   = refStates[i];
        int to   = NFAStatTran_[st][STATE_TRAN_MAX][0];
        int unit = NFAStatTran_[st][STATE_TRAN_MAX][1];

        RestoreRefStates(st, to, groupCapture_[unit].txtStart_, groupCapture_[unit].txtEnd_);
        states_[st].AppendType(State_Ref);
    }
#endif

    return !curStat.empty() && std::find(curStat.begin(), curStat.end(), accept) != curStat.end();
}

void RegExpNFA::SerializeState() const
{
}

void RegExpNFA::DeserializeState()
{
}

void RegExpNFA::ConvertToDFA(RegExpDFA& dfa) const
{

}

