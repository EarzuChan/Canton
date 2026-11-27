#include "QuickJsRuntimeContext.h"
#include "QuickJsPlatformApi.h"
#include "QuickJsValueReader.h"
#include <chrono>
#include <stdexcept>
#include <unordered_set>

namespace canton::quickjs {
    bool QuickJsRuntimeContext::pumpAssets() {
        for (auto it = assetRequests.begin(); it != assetRequests.end();) {
            if (it->bytes.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            ScopedValue resolve(context, it->resolve);
            ScopedValue reject(context, it->reject);
            JSValue value;
            bool ok = true;
            try {
                const auto bytes = it->bytes.get();
                value = it->text ? JS_NewStringLen(context, bytes.data(), bytes.size()) : JS_NewUint8ArrayCopy(context, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            } catch (const std::exception& error) {
                ok = false;
                value = JS_NewError(context);
                if (!JS_IsException(value)) JS_SetPropertyStr(context, value, "message", JS_NewString(context, error.what()));
            }
            it = assetRequests.erase(it);
            ScopedValue argument(context, value);
            if (JS_IsException(value)) return false;
            ScopedValue result(context, JS_Call(context, ok ? resolve.get() : reject.get(), JS_UNDEFINED, 1, &value));
            if (JS_IsException(result.get())) return false;
        }
        return true;
    }

    bool QuickJsRuntimeContext::pumpPlatformAsync() {
        // 1. 补发在 <App :onSecondInstance> 注册前缓冲的二次唤醒载荷
        if (!bufferedSecondInstances.empty() && JS_IsFunction(context, onSecondInstance)) {
            auto batch = std::move(bufferedSecondInstances);
            bufferedSecondInstances.clear();
            for (const auto& req : batch) {
                ScopedValue payload(context, QuickJsPlatformApi::makeLaunchRequestObject(context, req));
                JSValueConst argv[] = {payload.get()};
                ScopedValue fn(context, JS_DupValue(context, onSecondInstance));
                ScopedValue res(context, JS_Call(context, fn.get(), JS_UNDEFINED, 1, argv));
                if (JS_IsException(res.get())) return false;
            }
        }

        // 2. assets.exists
        for (auto it = assetExistsRequests.begin(); it != assetExistsRequests.end();) {
            if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            ScopedValue resolve(context, it->resolve);
            ScopedValue reject(context, it->reject);
            JSValue val = JS_UNDEFINED;
            bool ok = true;
            try {
                val = JS_NewBool(context, it->result.get());
            } catch (const std::exception& error) {
                ok = false;
                val = JS_NewError(context);
                if (!JS_IsException(val)) JS_SetPropertyStr(context, val, "message", JS_NewString(context, error.what()));
            }
            it = assetExistsRequests.erase(it);
            ScopedValue arg(context, val);
            if (JS_IsException(val)) return false;
            ScopedValue callRes(context, JS_Call(context, ok ? resolve.get() : reject.get(), JS_UNDEFINED, 1, &val));
            if (JS_IsException(callRes.get())) return false;
        }

        // 3. 异步文件系统
        for (auto it = fsRequests.begin(); it != fsRequests.end();) {
            if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            const auto op = it->op;
            ScopedValue resolve(context, it->resolve);
            ScopedValue reject(context, it->reject);
            FsBridgeResult fsRes;
            try {
                fsRes = it->result.get();
            } catch (const std::exception& error) {
                fsRes.ok = false;
                fsRes.errorCode = "IO";
                fsRes.errorMessage = error.what();
            }
            it = fsRequests.erase(it);

            JSValue obj = JS_NewObject(context);
            JS_SetPropertyStr(context, obj, "ok", JS_NewBool(context, fsRes.ok));
            if (!fsRes.ok) {
                JS_SetPropertyStr(context, obj, "errorCode", JS_NewString(context, fsRes.errorCode.c_str()));
                JS_SetPropertyStr(context, obj, "errorPath", JS_NewStringLen(context, fsRes.errorPath.data(), fsRes.errorPath.size()));
                JS_SetPropertyStr(context, obj, "errorMessage", JS_NewStringLen(context, fsRes.errorMessage.data(), fsRes.errorMessage.size()));
            } else if (op == "readText") {
                JS_SetPropertyStr(context, obj, "text", JS_NewStringLen(context, fsRes.bytes.data(), fsRes.bytes.size()));
            } else if (op == "readBytes") {
                JS_SetPropertyStr(context, obj, "bytes", JS_NewUint8ArrayCopy(context, reinterpret_cast<const uint8_t*>(fsRes.bytes.data()), fsRes.bytes.size()));
            } else if (op == "exists") {
                JS_SetPropertyStr(context, obj, "exists", JS_NewBool(context, fsRes.boolValue));
            } else if (op == "stat") {
                JS_SetPropertyStr(context, obj, "isFile", JS_NewBool(context, fsRes.isFile));
                JS_SetPropertyStr(context, obj, "isDirectory", JS_NewBool(context, fsRes.isDirectory));
                JS_SetPropertyStr(context, obj, "size", JS_NewFloat64(context, static_cast<double>(fsRes.size)));
                JS_SetPropertyStr(context, obj, "modifiedTimeMs", JS_NewFloat64(context, fsRes.modifiedTimeMs));
                JS_SetPropertyStr(context, obj, "createdTimeMs", JS_NewFloat64(context, fsRes.createdTimeMs));
            } else if (op == "readDir") {
                JSValue arr = JS_NewArray(context);
                for (std::uint32_t i = 0; i < fsRes.entries.size(); ++i) {
                    const auto& e = fsRes.entries[i];
                    JSValue item = JS_NewObject(context);
                    JS_SetPropertyStr(context, item, "name", JS_NewStringLen(context, e.name.data(), e.name.size()));
                    JS_SetPropertyStr(context, item, "isFile", JS_NewBool(context, e.isFile));
                    JS_SetPropertyStr(context, item, "isDirectory", JS_NewBool(context, e.isDirectory));
                    JS_SetPropertyUint32(context, arr, i, item);
                }
                JS_SetPropertyStr(context, obj, "entries", arr);
            }
            ScopedValue arg(context, obj);
            ScopedValue callRes(context, JS_Call(context, resolve.get(), JS_UNDEFINED, 1, &obj));
            if (JS_IsException(callRes.get())) return false;
        }

        // 4. 异步网络与流式下载进度
        for (auto it = netRequests.begin(); it != netRequests.end();) {
            if (it->progressInbox && JS_IsFunction(context, it->onProgress)) {
                NetBridgeProgress prog;
                bool emit = false;
                {
                    const std::scoped_lock lock(it->progressInbox->mutex);
                    if (it->progressInbox->dirty) {
                        prog = it->progressInbox->latest;
                        it->progressInbox->dirty = false;
                        emit = true;
                    }
                }
                if (emit) {
                    ScopedValue pObj(context, JS_NewObject(context));
                    JS_SetPropertyStr(context, pObj.get(), "receivedBytes", JS_NewFloat64(context, static_cast<double>(prog.receivedBytes)));
                    JS_SetPropertyStr(context, pObj.get(), "totalBytes", prog.totalBytes ? JS_NewFloat64(context, static_cast<double>(*prog.totalBytes)) : JS_NULL);
                    JSValueConst argv[] = {pObj.get()};
                    ScopedValue fn(context, JS_DupValue(context, it->onProgress));
                    ScopedValue res(context, JS_Call(context, fn.get(), JS_UNDEFINED, 1, argv));
                    if (JS_IsException(res.get())) return false;
                }
            }

            if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            const bool isDownload = it->isDownload;
            ScopedValue resolve(context, it->resolve);
            ScopedValue reject(context, it->reject);
            ScopedValue onProg(context, it->onProgress);
            NetBridgeResponse netRes;
            try {
                netRes = it->result.get();
            } catch (const std::exception& error) {
                netRes.ok = false;
                netRes.errorCode = "NETWORK";
                netRes.errorMessage = error.what();
            }
            it = netRequests.erase(it);

            JSValue obj = JS_NewObject(context);
            JS_SetPropertyStr(context, obj, "ok", JS_NewBool(context, netRes.ok));
            if (!netRes.ok) {
                JS_SetPropertyStr(context, obj, "errorCode", JS_NewString(context, netRes.errorCode.c_str()));
                JS_SetPropertyStr(context, obj, "errorUrl", JS_NewStringLen(context, netRes.errorUrl.data(), netRes.errorUrl.size()));
                JS_SetPropertyStr(context, obj, "errorMessage", JS_NewStringLen(context, netRes.errorMessage.data(), netRes.errorMessage.size()));
            } else if (isDownload) {
                JS_SetPropertyStr(context, obj, "status", JS_NewInt32(context, netRes.status));
                JS_SetPropertyStr(context, obj, "downloadOk", JS_NewBool(context, netRes.downloadOk));
                JS_SetPropertyStr(context, obj, "url", JS_NewStringLen(context, netRes.finalUrl.data(), netRes.finalUrl.size()));
                JS_SetPropertyStr(context, obj, "destPath", JS_NewStringLen(context, netRes.destPath.data(), netRes.destPath.size()));
                JS_SetPropertyStr(context, obj, "bytesWritten", JS_NewFloat64(context, static_cast<double>(netRes.bytesWritten)));
            } else {
                JS_SetPropertyStr(context, obj, "status", JS_NewInt32(context, netRes.status));
                JS_SetPropertyStr(context, obj, "url", JS_NewStringLen(context, netRes.finalUrl.data(), netRes.finalUrl.size()));
                JS_SetPropertyStr(context, obj, "contentType", JS_NewStringLen(context, netRes.contentType.data(), netRes.contentType.size()));
                JSValue hdrs = JS_NewObject(context);
                for (const auto& [k, v] : netRes.headers) {
                    JS_SetPropertyStr(context, hdrs, k.c_str(), JS_NewStringLen(context, v.data(), v.size()));
                }
                JS_SetPropertyStr(context, obj, "headers", hdrs);
                JS_SetPropertyStr(context, obj, "body", JS_NewUint8ArrayCopy(context, reinterpret_cast<const uint8_t*>(netRes.body.data()), netRes.body.size()));
            }
            ScopedValue arg(context, obj);
            ScopedValue callRes(context, JS_Call(context, resolve.get(), JS_UNDEFINED, 1, &obj));
            if (JS_IsException(callRes.get())) return false;
        }
        return true;
    }

    void QuickJsRuntimeContext::freePlatformHandles() {
        if (!context) return;
        for (auto* val : {&onGraphicsEnvChanged, &onSecondInstance, &onTrayEvent, &onTaskbarAction, &onMediaButton}) {
            JS_FreeValue(context, *val);
            *val = JS_UNDEFINED;
        }
        for (auto& [_, entry] : timers) {
            JS_FreeValue(context, entry.callback);
            for (auto& arg : entry.args) JS_FreeValue(context, arg);
        }
        timers.clear();
        fallbackTimerDue.clear();
        if (platform && platform->clearTimers) platform->clearTimers();

        for (auto& req : assetExistsRequests) {
            JS_FreeValue(context, req.resolve);
            JS_FreeValue(context, req.reject);
        }
        assetExistsRequests.clear();

        for (auto& req : fsRequests) {
            JS_FreeValue(context, req.resolve);
            JS_FreeValue(context, req.reject);
        }
        fsRequests.clear();

        for (auto& req : netRequests) {
            if (platform && platform->netCancel) platform->netCancel(req.reqId);
            JS_FreeValue(context, req.resolve);
            JS_FreeValue(context, req.reject);
            JS_FreeValue(context, req.onProgress);
        }
        netRequests.clear();
    }

    void QuickJsRuntimeContext::beginRearrange() {
        if (rearrangeCheckpoint) throw std::logic_error("重排候选不能重入");
        if (!pendingTransactions) throw std::logic_error("重排缺少原生提交队列");
        std::unordered_map<canton::core::WindowId, std::optional<canton::core::MutationTransaction>> savedWindowPending;
        for (const auto& [windowId, queue] : windowTransactions) savedWindowPending.emplace(windowId, queue.pending());
        rearrangeCheckpoint = RearrangeCheckpoint{
            rootNodeId,
            pendingTransactions->pending(),
            std::move(savedWindowPending),
            pendingAppConfig,
            windowMutations,
            nodeWindows,
            bindings,
            publishedModifiers,
            modifierInputs,
            nodeTypes,
            measurePolicies,
            nodeGenerations,
            hostBindings,
            modifierBindings,
            childrenByNode,
            parentByNode,
        };
        rearrangeSubmission = std::make_shared<canton::core::RearrangeSubmission>(canton::core::RearrangeSubmission{canton::core::allocateRuntimeIdentity(), false});
        submittedRearrange.reset();
        events.beginCandidate();
    }

    void QuickJsRuntimeContext::abortRearrange() {
        if (!rearrangeCheckpoint) return;
        if (rearrangeSubmission) rearrangeSubmission->cancelled = true;
        rearrangeSubmission.reset();
        submittedRearrange.reset();
        auto saved = std::move(*rearrangeCheckpoint);
        rearrangeCheckpoint.reset();
        rootNodeId = saved.root;
        pendingTransactions->clear();
        if (saved.pending) pendingTransactions->push(std::move(*saved.pending));
        windowTransactions.clear();
        for (auto& [windowId, pending] : saved.savedWindowPending) {
            if (pending) windowTransactions[windowId].push(std::move(*pending));
        }
        pendingAppConfig = std::move(saved.savedAppConfig);
        windowMutations = std::move(saved.savedWindowMutations);
        nodeWindows = std::move(saved.savedNodeWindows);
        bindings = std::move(saved.savedBindings);
        publishedModifiers = std::move(saved.savedPublishedModifiers);
        modifierInputs = std::move(saved.savedModifierInputs);
        nodeTypes = std::move(saved.savedNodeTypes);
        measurePolicies = std::move(saved.savedMeasurePolicies);
        nodeGenerations = std::move(saved.savedNodeGenerations);
        hostBindings = std::move(saved.savedHostBindings);
        modifierBindings = std::move(saved.savedModifierBindings);
        childrenByNode = std::move(saved.savedChildrenByNode);
        parentByNode = std::move(saved.savedParentByNode);
        events.abortCandidate();
    }

    void QuickJsRuntimeContext::commitRearrange() {
        rearrangeSubmission.reset();
        submittedRearrange.reset();
        rearrangeCheckpoint.reset();
        events.commitCandidate();
    }

    canton::core::WindowId QuickJsRuntimeContext::windowOfNode(canton::core::NodeId id) const noexcept {
        const auto found = nodeWindows.find(id);
        return found == nodeWindows.end() ? 0 : found->second;
    }

    canton::core::MutationTransaction* QuickJsRuntimeContext::currentTransaction() noexcept {
        return pendingTransactions == nullptr ? nullptr : &pendingTransactions->ensurePending();
    }

    canton::core::MutationTransaction* QuickJsRuntimeContext::transactionForWindow(canton::core::WindowId windowId) noexcept {
        if (windowId == 0) return currentTransaction();
        auto& tx = windowTransactions[windowId].ensurePending();
        if (rearrangeSubmission && !JS_IsUndefined(rearrangeCompletion)) tx.rearrange = rearrangeSubmission;
        return &tx;
    }

    canton::core::MutationTransaction* QuickJsRuntimeContext::transactionForNode(canton::core::NodeId id) noexcept {
        return transactionForWindow(windowOfNode(id));
    }

    void QuickJsRuntimeContext::stampRearrangeSubmission() {
        submittedRearrange = rearrangeSubmission;
        if (windowTransactions.empty() && windowMutations.empty()) {
            if (pendingTransactions) pendingTransactions->ensurePending().rearrange = rearrangeSubmission;
            return;
        }
        if (pendingTransactions && pendingTransactions->hasPending()) {
            pendingTransactions->ensurePending().rearrange = rearrangeSubmission;
        }
        for (auto& [_, queue] : windowTransactions) {
            if (queue.hasPending()) queue.ensurePending().rearrange = rearrangeSubmission;
        }
    }

    void QuickJsRuntimeContext::push(canton::core::NodeId ownerNode, canton::core::TreeMutation mutation) {
        if (auto* transaction = transactionForNode(ownerNode)) transaction->operations.emplace_back(std::move(mutation));
    }

    canton::core::BindingHandle QuickJsRuntimeContext::registerBinding(canton::core::BindingTarget target) {
        const auto nodeId = std::visit([](const auto& item) { return item.node.id; }, target);
        const canton::core::BindingHandle handle{canton::core::allocateRuntimeIdentity(), 1};
        bindings.emplace(handle.identity, canton::core::RegisterBinding{handle, target});
        if (auto* transaction = transactionForNode(nodeId)) transaction->operations.emplace_back(canton::core::RegisterBinding{handle, std::move(target)});
        return handle;
    }

    std::vector<const canton::core::ModifierDescriptor*> QuickJsRuntimeContext::modifierInputDescriptors(canton::core::NodeId id) const {
        std::vector<const canton::core::ModifierDescriptor*> result;
        if (const auto found = modifierInputs.find(id); found != modifierInputs.end()) {
            result.reserve(found->second.size());
            for (const auto& input : found->second) result.push_back(&input.descriptor);
        }
        return result;
    }

    void QuickJsRuntimeContext::updateBinding(canton::core::BindingHandle handle, canton::core::SlotValue value) {
        const auto& target = bindings.at(handle.identity).target;
        const auto nodeId = std::visit([](const auto& item) { return item.node.id; }, target);
        if (const auto* chain = std::get_if<canton::core::ModifierChainTarget>(&target)) {
            const auto& descriptors = std::get<canton::core::ModifierDescriptors>(value);
            const auto previous = modifierInputDescriptors(chain->node.id);
            const auto matches = canton::core::matchModifierDescriptors(previous, descriptors);
            auto& inputs = modifierInputs[chain->node.id];
            std::vector<PublishedModifier> next;
            next.reserve(descriptors.size());
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                const auto match = matches[index];
                const auto receiver = match < inputs.size() ? inputs[match].handle : canton::core::ModifierHandle{};
                next.push_back({chain->node, receiver, descriptors[index], index});
            }
            inputs = std::move(next);
        } else if (const auto* instance = std::get_if<canton::core::ModifierInputTarget>(&target)) {
            for (auto& input : modifierInputs.at(instance->node.id)) {
                if (input.handle == instance->modifier) {
                    input.descriptor.value = std::get<canton::core::ModifierValue>(value);
                    break;
                }
            }
        }
        if (auto* transaction = transactionForNode(nodeId)) transaction->operations.emplace_back(canton::core::SlotUpdate{handle, std::move(value)});
    }

    void QuickJsRuntimeContext::retireBinding(canton::core::BindingHandle handle) {
        const auto found = bindings.find(handle.identity);
        if (found == bindings.end() || found->second.handle != handle) return;
        const auto nodeId = std::visit([](const auto& item) { return item.node.id; }, found->second.target);
        std::visit(
            [&](const auto& target) {
                using T = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<T, canton::core::HostInputTarget>) {
                    if (auto node = hostBindings.find(target.node.id); node != hostBindings.end()) {
                        auto input = node->second.find(target.input);
                        if (input != node->second.end() && input->second == handle) node->second.erase(input);
                    }
                } else if constexpr (std::is_same_v<T, canton::core::ModifierChainTarget>) {
                    if (auto input = modifierBindings.find(target.node.id); input != modifierBindings.end() && input->second == handle) modifierBindings.erase(input);
                }
            },
            found->second.target);
        bindings.erase(found);
        if (auto* transaction = transactionForNode(nodeId)) transaction->operations.emplace_back(canton::core::RetireBinding{handle});
    }

    void QuickJsRuntimeContext::setHostInput(canton::core::NodeId id, canton::core::HostInput input, canton::core::PropValue value) {
        auto& handle = hostBindings[id][input];
        if (!handle.valid()) handle = registerBinding(canton::core::HostInputTarget{{id, nodeGenerations.at(id)}, input});
        updateBinding(handle, std::move(value));
    }

    void QuickJsRuntimeContext::setModifierChain(canton::core::NodeId id, canton::core::ModifierDescriptors value) {
        auto& handle = modifierBindings[id];
        if (!handle.valid()) handle = registerBinding(canton::core::ModifierChainTarget{{id, nodeGenerations.at(id)}});
        updateBinding(handle, std::move(value));
    }

    void QuickJsRuntimeContext::attachChild(canton::core::NodeId parent, canton::core::NodeId child, std::uint32_t index) {
        if (parent == 0 || child == 0) return;
        if (windowOfNode(parent) != windowOfNode(child)) throw std::logic_error("不能跨窗口挂载布局节点");
        if (const auto oldParent = parentByNode.find(child); oldParent != parentByNode.end()) detachChild(oldParent->second, child);
        auto& children = childrenByNode[parent];
        children.erase(std::remove(children.begin(), children.end(), child), children.end());
        const auto insertIndex = std::min<std::size_t>(index, children.size());
        children.insert(children.begin() + static_cast<std::ptrdiff_t>(insertIndex), child);
        parentByNode[child] = parent;
    }

    void QuickJsRuntimeContext::detachChild(canton::core::NodeId parent, canton::core::NodeId child) {
        if (const auto childrenIt = childrenByNode.find(parent); childrenIt != childrenByNode.end()) {
            auto& children = childrenIt->second;
            children.erase(std::remove(children.begin(), children.end(), child), children.end());
            if (children.empty()) childrenByNode.erase(childrenIt);
        }
        if (const auto parentIt = parentByNode.find(child); parentIt != parentByNode.end() && parentIt->second == parent) parentByNode.erase(parentIt);
    }

    void QuickJsRuntimeContext::retireSubtree(canton::core::NodeId id) {
        const auto windowId = windowOfNode(id);
        std::vector<canton::core::NodeId> pending{id};
        std::unordered_set<canton::core::NodeId> retiredNodes;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!retiredNodes.insert(current).second) continue;
            if (const auto children = childrenByNode.find(current); children != childrenByNode.end()) pending.insert(pending.end(), children->second.begin(), children->second.end());
        }

        std::vector<canton::core::BindingHandle> retiredBindings;
        for (const auto& [_, binding] : bindings) {
            if (std::visit([&](const auto& target) { return retiredNodes.contains(target.node.id); }, binding.target)) retiredBindings.push_back(binding.handle);
        }
        for (const auto handle : retiredBindings) retireBinding(handle);

        events.releaseNodes(retiredNodes, transactionForWindow(windowId));
        if (const auto parent = parentByNode.find(id); parent != parentByNode.end()) detachChild(parent->second, id);

        for (const auto current : retiredNodes) {
            hostBindings.erase(current);
            modifierBindings.erase(current);
            modifierInputs.erase(current);
            nodeGenerations.erase(current);
            nodeTypes.erase(current);
            measurePolicies.erase(current);
            childrenByNode.erase(current);
            parentByNode.erase(current);
            nodeWindows.erase(current);
        }
    }

    void QuickJsRuntimeContext::recordToast(QuickJsToastRequest toast) {
        diagnosticToasts.push_back(std::move(toast));
    }

    void QuickJsRuntimeContext::recordDiagnosticAction(QuickJsDiagnosticAction action) {
        diagnosticActions.push_back(std::move(action));
        while (diagnosticActions.size() > 64) diagnosticActions.erase(diagnosticActions.begin());
    }
}
