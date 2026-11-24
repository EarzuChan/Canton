import { isIdentifier } from '@canton/shared'
import { ElementTypes, NodeTypes, type RootNode, type SimpleExpressionNode, type SourceLocation, type TemplateChildNode } from './ast.ts'
import type { CompilerOptions } from './options.ts'

// 在结构转换之前检查源码契约，不能让被转换掉的指令绕过校验
export function validateTemplateContract(root: RootNode, options: CompilerOptions): string[] { // Options 未被使用喵
    const slots = new Set<string>()

    const fail: (message: string, loc: SourceLocation) => never = (message, loc) => { throw Object.assign(new SyntaxError(message), { code: 'CANTON_TEMPLATE', loc }) }

    const visit = (children: TemplateChildNode[]) => {
        for (let index = children.length - 1; index >= 0; index--) {
            const node = children[index]
            if (node.type === NodeTypes.TEXT) {
                if (node.content.trim()) fail('模板内容不接受裸文本，请使用 Text 的 text 参数', node.loc)
                children.splice(index, 1)
                continue
            }
            if (node.type === NodeTypes.INTERPOLATION) fail('模板内容不接受插值，请使用 Text 的 text 参数', node.loc)
            if (node.type !== NodeTypes.ELEMENT) continue

            if (!/^[A-Z][A-Za-z0-9]*$/.test(node.tag)) fail(`Arrangable 标签必须使用准确的 PascalCase 名称：${node.tag}`, node.loc)
            const names = new Set<string>()
            for (const prop of node.props) {
                if (prop.type === NodeTypes.DIRECTIVE) {
                    if (!['bind', 'if', 'else-if', 'else', 'for', 'slot', 'param'].includes(prop.name)) fail(`Canton 模板不支持 c-${prop.name}`, prop.loc)
                    if (prop.name === 'bind' && (!prop.arg || prop.arg.type === NodeTypes.SIMPLE_EXPRESSION && !prop.arg.content.trim())) fail('参数绑定必须提供参数名', prop.loc)
                    if (prop.name === 'bind' && (!prop.exp || prop.exp.type === NodeTypes.SIMPLE_EXPRESSION && !prop.exp.content.trim())) fail('参数绑定必须显式提供表达式', prop.loc)
                    if (prop.name === 'slot' && (node.tag !== 'Template' || prop.exp || prop.arg && (prop.arg.type !== NodeTypes.SIMPLE_EXPRESSION || !prop.arg.isStatic || !isIdentifier(prop.arg.content)))) fail('具名内容必须使用 Template 和合法槽位标识符', prop.loc)
                    if (prop.name === 'param') {
                        // 默认槽的参数绑在 Arrangable 标签上，具名槽的参数绑在选择该槽位的 Template 上
                        if (node.tag === 'Slot' || node.tag === 'Template' && !node.props.some(other => other.type === NodeTypes.DIRECTIVE && other.name === 'slot')) fail('槽位参数只能绑定在 Arrangable 标签或 <Template #name> 上', prop.loc)
                        const name = (prop.arg as SimpleExpressionNode).content
                        if (!isIdentifier(name)) fail(`槽位参数名不是合法标识符：${name}`, prop.loc)
                    }
                }

                const name = prop.type === NodeTypes.ATTRIBUTE ? prop.name : prop.name === 'bind' && prop.arg?.type === NodeTypes.SIMPLE_EXPRESSION && prop.arg.isStatic ? prop.arg.content : undefined
                if (name === undefined) continue
                if (!isIdentifier(name)) fail(`参数名称必须是合法标识符：${name}`, prop.loc)
                if (names.has(name)) fail(`重复参数：${name}`, prop.loc)
                names.add(name)
            }

            if (node.tag === 'Slot') {
                let name = 'default'

                // 出口除静态 name 与 key 外，其余具名参数都是交付给内容的实参
                for (const prop of node.props) {
                    if (prop.type === NodeTypes.ATTRIBUTE) {
                        if (prop.name !== 'name') continue
                        const value = prop.value?.content
                        if (!value || !isIdentifier(value)) fail(`Slot 的 name 必须是合法标识符：${value ?? ''}`, prop.loc)
                        name = value
                    } else if (prop.name === 'bind' && !(prop.arg?.type === NodeTypes.SIMPLE_EXPRESSION && prop.arg.isStatic && prop.arg.content !== 'name')) fail('Slot 出口只接受静态 name 和具名实参', prop.loc)
                }

                if (node.children.some(child => child.type !== NodeTypes.COMMENT && !(child.type === NodeTypes.TEXT && !child.content.trim()))) fail('Slot 未被提供时为空内容，不接受默认子内容', node.loc)
                slots.add(name)
                node.tagType = ElementTypes.SLOT
            } else if (node.tag === 'Template') node.tagType = ElementTypes.TEMPLATE

            visit(node.children)
        }
    }

    visit(root.children)
    return [...slots]
}
