
static void transmute_goto(TB_Passes* restrict opt, TB_Function* f, TB_Node* br, TB_Node* dst) {
    assert(br->type == TB_BRANCH && dst->input_count >= 1);

    // convert to unconditional branch
    set_input(opt, br, NULL, 1);
    br->input_count = 1;

    // remove predecessor from other branches
    TB_Node* bb = unsafe_get_region(br);
    TB_NodeBranch* br_info = TB_NODE_GET_EXTRA(br);
    FOREACH_N(i, 0, br_info->succ_count) {
        remove_pred(opt, f, bb, br_info->succ[i]);
    }
    br_info->succ_count = 1;

    // construct new projection for the branch
    TB_Node* proj = tb_alloc_node(f, TB_PROJ, TB_TYPE_CONTROL, 1, sizeof(TB_NodeProj));
    set_input(opt, proj, br, 0);
    TB_NODE_SET_EXTRA(proj, TB_NodeProj, .index = 0);

    dst->input_count = 1;
    set_input(opt, dst, proj, 0);

    // set new successor
    br_info->succ_count = 1;
    br_info->succ[0] = dst;

    // we need to mark the changes to that jump
    // threading can clean it up
    tb_pass_mark(opt, bb);
    tb_pass_mark(opt, proj);
    tb_pass_mark(opt, dst);
    tb_pass_mark_users(opt, bb);

    CUIK_TIMED_BLOCK("recompute order") {
        tb_function_free_postorder(&opt->order);
        opt->order = tb_function_get_postorder(f);
    }
}

static TB_Node* ideal_phi(TB_Passes* restrict opt, TB_Function* f, TB_Node* n) {
    // if branch, both paths are empty => select(cond, t, f)
    TB_DataType dt = n->dt;
    TB_Node* region = n->inputs[0];
    if (region->input_count == 2) {
        // for now we'll leave multi-phi scenarios alone, we need
        // to come up with a cost-model around this stuff.
        int phi_count = 0;
        for (User* use = find_users(opt, region); use; use = use->next) {
            if (use->n->type == TB_PHI) phi_count++;
            if (phi_count > 1) return NULL;
        }

        // guarentee paths are effectless
        if (!is_empty_bb(opt, region->inputs[0]->inputs[0])) { return NULL; }
        if (!is_empty_bb(opt, region->inputs[1]->inputs[0])) { return NULL; }

        // these don't have directions, i just need names
        TB_Node* left  = region->inputs[0]->inputs[0]->inputs[0];
        TB_Node* right = region->inputs[1]->inputs[0]->inputs[0];

        // is it a proper if-diamond?
        if (left->input_count == 1 && right->input_count == 1 &&
            left->inputs[0]->inputs[0]->type == TB_BRANCH &&
            left->inputs[0]->inputs[0] == right->inputs[0]->inputs[0]) {
            TB_Node* branch = left->inputs[0]->inputs[0];
            TB_NodeBranch* header_br = TB_NODE_GET_EXTRA(branch);

            if (header_br->succ_count == 2) {
                assert(left->inputs[0]->inputs[0]->input_count == 2);
                TB_Node* cond    = branch->inputs[1];
                TB_Node* left_v  = n->inputs[1];
                TB_Node* right_v = n->inputs[2];

                bool right_false = header_br->succ[0] == right;
                uint64_t falsey = TB_NODE_GET_EXTRA_T(branch, TB_NodeBranch)->keys[0];

                // TODO(NeGate): handle non-zero falseys
                if (falsey == 0) {
                    // header -> merge
                    transmute_goto(opt, f, branch, region);

                    TB_Node* selector = tb_alloc_node(f, TB_SELECT, dt, 4, 0);
                    set_input(opt, selector, cond, 1);
                    set_input(opt, selector, left_v, 2 + right_false);
                    set_input(opt, selector, right_v, 2 + !right_false);

                    return selector;
                }
            }
        }
    }

    return NULL;
}

static TB_Node* ideal_branch(TB_Passes* restrict opt, TB_Function* f, TB_Node* n) {
    TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);

    if (br->succ_count == 2) {
        if (n->input_count == 2 && br->keys[0] == 0) {
            TB_Node* cmp_node = n->inputs[1];
            TB_NodeTypeEnum cmp_type = cmp_node->type;

            // empty BB, just does if branch but the condition is effect-less
            // if (a && b) A else B => if (a ? b : 0) A else B
            if (is_empty_bb(opt, n)) {
                TB_Node* bb = n->inputs[0];
                assert(bb->type == TB_REGION || bb->type == TB_START);

                uint64_t falsey = br->keys[0];
                TB_Node* pred_branch = bb->inputs[0]->inputs[0];

                // needs one pred
                uint64_t pred_falsey;
                if (bb->input_count == 1 && is_if_branch(pred_branch, &pred_falsey)) {
                    TB_NodeBranch* pred_br_info = TB_NODE_GET_EXTRA(pred_branch);

                    bool bb_on_false = pred_br_info->succ[0] != bb;
                    TB_Node* shared_edge = pred_br_info->succ[!bb_on_false];

                    int shared_i = -1;
                    if (shared_edge == pred_br_info->succ[0]) shared_i = 0;
                    if (shared_edge == pred_br_info->succ[1]) shared_i = 1;

                    // TODO(NeGate): implement form which works on an arbitrary falsey
                    if (falsey == 0 && shared_i >= 0) {
                        TB_Node* pred_cmp = pred_branch->inputs[1];

                        // convert first branch into an unconditional into bb
                        transmute_goto(opt, f, pred_branch, bb);

                        // we wanna normalize into a comparison (not a boolean -> boolean)
                        if (!(pred_cmp->dt.type == TB_INT && pred_cmp->dt.data == 1)) {
                            assert(pred_cmp->dt.type != TB_FLOAT && "TODO");
                            TB_Node* imm = make_int_node(f, opt, pred_cmp->dt, pred_falsey);
                            tb_pass_mark(opt, imm);

                            TB_Node* new_node = tb_alloc_node(f, TB_CMP_NE, TB_TYPE_BOOL, 3, sizeof(TB_NodeCompare));
                            set_input(opt, new_node, pred_cmp, 1);
                            set_input(opt, new_node, imm, 2);
                            TB_NODE_SET_EXTRA(new_node, TB_NodeCompare, .cmp_dt = pred_cmp->dt);

                            tb_pass_mark(opt, new_node);
                            pred_cmp = new_node;
                        }

                        TB_Node* false_node = make_int_node(f, opt, n->inputs[1]->dt, falsey);
                        tb_pass_mark(opt, false_node);

                        // a ? b : 0
                        TB_Node* selector = tb_alloc_node(f, TB_SELECT, n->inputs[1]->dt, 4, 0);
                        set_input(opt, selector, pred_cmp, 1);
                        set_input(opt, selector, n->inputs[1], 2 + bb_on_false);
                        set_input(opt, selector, false_node, 2 + !bb_on_false);

                        set_input(opt, n, selector, 1);
                        tb_pass_mark(opt, selector);
                        return n;
                    }
                }
            }

            // br ((y <= x)) => br (x < y) flipped conditions
            if (cmp_type == TB_CMP_SLE || cmp_type == TB_CMP_ULE) {
                TB_Node* new_cmp = tb_alloc_node(f, cmp_type == TB_CMP_SLE ? TB_CMP_SLT : TB_CMP_ULT, TB_TYPE_BOOL, 3, sizeof(TB_NodeCompare));
                set_input(opt, new_cmp, cmp_node->inputs[2], 1);
                set_input(opt, new_cmp, cmp_node->inputs[1], 2);
                TB_NODE_SET_EXTRA(new_cmp, TB_NodeCompare, .cmp_dt = TB_NODE_GET_EXTRA_T(cmp_node, TB_NodeCompare)->cmp_dt);

                SWAP(TB_Node*, br->succ[0], br->succ[1]);
                set_input(opt, n, new_cmp, 1);
                tb_pass_mark(opt, new_cmp);
                return n;
            }

            // br ((x != y) != 0) => br (x != y)
            if ((cmp_type == TB_CMP_NE || cmp_type == TB_CMP_EQ) && cmp_node->inputs[2]->type == TB_INTEGER_CONST) {
                TB_NodeInt* i = TB_NODE_GET_EXTRA(cmp_node->inputs[2]);
                if (i->num_words == 1) {
                    set_input(opt, n, cmp_node->inputs[1], 1);
                    br->keys[0] = i->words[0];

                    // flip successors
                    if (cmp_type == TB_CMP_EQ) {
                        SWAP(TB_Node*, br->succ[0], br->succ[1]);
                    }
                    return n;
                }
            }
        }
    }

    return NULL;
}

bool tb_pass_cfg(TB_Passes* opt) {
    #if 0
    // walk dominators to see if we've already checked this condition
    TB_Node* other_bb = bb;
    while (0 && other_bb != f->start_node) retry: {
        other_bb = idom(opt->doms, other_bb);

        TB_NodeRegion* other_region = TB_NODE_GET_EXTRA(other_bb);
        TB_Node* latch = other_region->end;
        if (latch->type != TB_BRANCH) break;
        if (latch->input_count != 2) continue;

        bool hit = false;
        ptrdiff_t j = -1;
        FOREACH_N(i, 0, other_region->succ_count) {
            if (tb_is_dominated_by(opt->doms, other_region->succ[i], bb)) {
                // duplicates means ambiguous which we can't know much about
                if (j >= 0) goto retry;
                j = i;
            }
        }

        TB_NodeBranch* latch_br = TB_NODE_GET_EXTRA(latch);
        if (j >= 0 && latch->inputs[1] == n->inputs[1] && latch_br->keys[0] == br->keys[0]) {
            TB_Node* dead_block = region->succ[1 - j];

            if (dead_block->input_count == 1) {
                // convert conditional into goto
                set_input(opt, n, NULL, 1);
                n->input_count = 1;
                region->succ_count = 1;
                region->succ[0] = region->succ[j];

                set_input(opt, dead_block, NULL, 0);
                dead_block->input_count = 0;

                // remove predecessor from dead_block's successors
                TB_NodeRegion* dead_region = TB_NODE_GET_EXTRA(dead_block);
                FOREACH_N(k, 0, dead_region->succ_count) {
                    remove_pred(opt, f, dead_block, dead_region->succ[k]);
                }

                return n;
            }
        }
    }
    #endif

    return false;
}
