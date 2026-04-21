//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

import { LiteGraph } from 'litegraph.js';

const IGNORED_TAGS = new Set([
    'input',
    'output',
    'look',
    'materialassign',
    'collection',
    'propertyset',
    'propertysetassign',
    'geominfo',
    'token',
    'typedef',
    'nodedef',
    'implementation'
]);

export class MtlxGraphView
{
    constructor(graphCanvasId)
    {
        this._graphCanvasId = graphCanvasId;
        this._editor = null;
        this._graph = new LiteGraph.LGraph();
        this._canvas = new LiteGraph.LGraphCanvas(`#${graphCanvasId}`, this._graph);
        this._canvas.background_image = null;
        this._canvas.ds.scale = 0.85;

        this._canvas.onNodeSelected = (node) => this._onNodeSelected(node);
    }

    setEditor(editor)
    {
        this._editor = editor;
    }

    setVisible(visible)
    {
        const panel = document.getElementById('graph-panel');
        if (panel)
        {
            panel.style.display = visible ? 'block' : 'none';
        }

        if (visible)
        {
            this._graph.start();
            this.resize();
        }
        else
        {
            this._graph.stop();
        }
    }

    resize()
    {
        if (this._canvas)
        {
            this._canvas.resize();
            this._canvas.setDirty(true, true);
        }
    }

    async loadFromFile(loader, materialFilename)
    {
        if (!materialFilename)
        {
            this._clearGraph('No material selected');
            return;
        }

        const xml = await new Promise((resolve, reject) =>
        {
            loader.load(materialFilename, data => resolve(data), null, reject);
        });

        if (typeof xml !== 'string' || xml.length === 0)
        {
            this._clearGraph('Unable to read .mtlx source');
            return;
        }

        this.loadFromXml(xml, materialFilename);
    }

    loadFromXml(xmlString, caption = 'MaterialX')
    {
        this._graph.clear();

        const parser = new DOMParser();
        const xmlDoc = parser.parseFromString(xmlString, 'application/xml');
        const parseError = xmlDoc.querySelector('parsererror');
        if (parseError)
        {
            this._clearGraph('Invalid .mtlx XML');
            return;
        }

        const root = xmlDoc.documentElement;
        if (!root)
        {
            this._clearGraph('Empty .mtlx document');
            return;
        }

        const graphNodeMap = new Map();
        const graphInputMap = new Map();
        const pendingLinks = [];

        const nodeElements = this._collectNodeElements(root);
        if (!nodeElements.length)
        {
            this._clearGraph('No nodes found in .mtlx');
            return;
        }

        this._addDocumentTitleNode(caption);

        let column = 0;
        let row = 0;
        const rowSize = 6;
        for (const element of nodeElements)
        {
            const scope = this._getScopeName(element);
            const name = element.getAttribute('name') || element.tagName;
            const key = `${scope}/${name}`;

            const graphNode = LiteGraph.createNode('basic/watch');
            graphNode.title = `${element.tagName}:${name}`;
            graphNode._mtlxElement = element;
            graphNode.size = [220, 80];

            const inputs = Array.from(element.children).filter(child => child.tagName === 'input');
            const outputs = Array.from(element.children).filter(child => child.tagName === 'output');

            if (!inputs.length)
            {
                graphNode.addInput('in', 'any');
            }
            else
            {
                for (const input of inputs)
                {
                    graphNode.addInput(input.getAttribute('name') || 'input', 'any');
                }
            }

            if (!outputs.length)
            {
                graphNode.addOutput('out', 'any');
            }
            else
            {
                for (const output of outputs)
                {
                    graphNode.addOutput(output.getAttribute('name') || 'output', 'any');
                }
            }

            graphNode.pos = [80 + column * 270, 80 + row * 150];
            this._graph.add(graphNode);
            graphNodeMap.set(key, { node: graphNode, element, scope });

            this._collectPendingLinks(element, scope, key, inputs, pendingLinks, graphInputMap);

            column++;
            if (column >= rowSize)
            {
                column = 0;
                row++;
            }
        }

        for (const link of pendingLinks)
        {
            const source = this._findSourceNode(link, graphNodeMap, graphInputMap);
            const target = graphNodeMap.get(link.targetNodeKey);
            if (!source || !target)
            {
                continue;
            }

            const sourceSlot = this._findOutputSlot(source.node, link.sourceOutputName);
            const targetSlot = this._findInputSlot(target.node, link.targetInputName);
            if (sourceSlot < 0 || targetSlot < 0)
            {
                continue;
            }

            source.node.connect(sourceSlot, target.node, targetSlot);
        }

        this._graph.arrange(120);
        this._graph.start();
        this._canvas.setDirty(true, true);
    }

    _addDocumentTitleNode(caption)
    {
        const titleNode = LiteGraph.createNode('basic/string');
        titleNode.title = 'MaterialX Document';
        titleNode.properties.value = caption;
        titleNode.pos = [40, 20];
        titleNode.size = [320, 60];
        this._graph.add(titleNode);
    }

    _collectNodeElements(root)
    {
        const candidates = root.querySelectorAll('[name]');
        return Array.from(candidates).filter(element =>
        {
            if (IGNORED_TAGS.has(element.tagName))
            {
                return false;
            }

            const hasInputs = element.querySelector(':scope > input') != null;
            const hasOutputs = element.querySelector(':scope > output') != null;
            const isGraph = element.tagName === 'nodegraph';
            const hasType = element.hasAttribute('type');

            return hasInputs || hasOutputs || isGraph || hasType;
        });
    }

    _collectPendingLinks(element, scope, targetNodeKey, inputs, pendingLinks, graphInputMap)
    {
        for (const input of inputs)
        {
            const targetInputName = input.getAttribute('name') || 'input';
            const sourceNodeName = input.getAttribute('nodename');
            const sourceOutputName = input.getAttribute('output') || 'out';

            if (sourceNodeName)
            {
                pendingLinks.push({
                    sourceNodeName,
                    sourceOutputName,
                    sourceScope: scope,
                    targetNodeKey,
                    targetInputName
                });
                continue;
            }

            const interfaceName = input.getAttribute('interfacename');
            if (interfaceName)
            {
                const interfaceKey = `${scope}/__interface__${interfaceName}`;
                let graphInputNode = graphInputMap.get(interfaceKey);
                if (!graphInputNode)
                {
                    graphInputNode = LiteGraph.createNode('basic/const');
                    graphInputNode.title = `interface:${interfaceName}`;
                    graphInputNode.addOutput(interfaceName, 'any');
                    graphInputNode.pos = [20, 120 + graphInputMap.size * 90];
                    this._graph.add(graphInputNode);
                    graphInputMap.set(interfaceKey, { node: graphInputNode, outputName: interfaceName, scope });
                }

                pendingLinks.push({
                    sourceNodeName: `__interface__${interfaceName}`,
                    sourceOutputName: interfaceName,
                    sourceScope: scope,
                    targetNodeKey,
                    targetInputName,
                    isInterface: true
                });
            }
        }
    }

    _findSourceNode(link, graphNodeMap, graphInputMap)
    {
        if (link.isInterface)
        {
            return graphInputMap.get(`${link.sourceScope}/${link.sourceNodeName}`) || null;
        }

        const scoped = graphNodeMap.get(`${link.sourceScope}/${link.sourceNodeName}`);
        if (scoped)
        {
            return scoped;
        }

        return graphNodeMap.get(`root/${link.sourceNodeName}`) || null;
    }

    _findInputSlot(node, name)
    {
        if (!node.inputs || !node.inputs.length)
        {
            return -1;
        }

        let index = node.inputs.findIndex(slot => slot.name === name);
        if (index < 0)
        {
            index = 0;
        }
        return index;
    }

    _findOutputSlot(node, name)
    {
        if (!node.outputs || !node.outputs.length)
        {
            return -1;
        }

        let index = node.outputs.findIndex(slot => slot.name === name);
        if (index < 0)
        {
            index = 0;
        }
        return index;
    }

    _getScopeName(element)
    {
        let current = element.parentElement;
        while (current)
        {
            if (current.tagName === 'nodegraph')
            {
                return current.getAttribute('name') || 'root';
            }
            current = current.parentElement;
        }
        return 'root';
    }

    _onNodeSelected(node)
    {
        if (!this._editor) return;

        this._editor.initialize();
        const gui = this._editor.getGUI();

        const element = node._mtlxElement;
        if (!element)
        {
            gui.open();
            return;
        }

        const nodeName = element.getAttribute('name') || element.tagName;
        const folder = gui.addFolder(`${element.tagName}: ${nodeName}`);

        const inputs = Array.from(element.children).filter(c => c.tagName === 'input');
        for (const input of inputs)
        {
            const inputName = input.getAttribute('name') || 'input';
            const type = input.getAttribute('type') || '';
            const valStr = input.getAttribute('value');
            const nodename = input.getAttribute('nodename');
            const interfacename = input.getAttribute('interfacename');

            if (nodename || interfacename || valStr === null)
            {
                // Connected or valueless input — show read-only
                const label = nodename ? `\u2192 ${nodename}` : interfacename ? `\u2194 ${interfacename}` : `(${type})`;
                const obj = {};
                obj[inputName] = label;
                folder.add(obj, inputName).name(inputName).disable();
            }
            else
            {
                this._addInputControl(folder, input, inputName, type, valStr);
            }
        }

        gui.open();
        folder.open();
    }

    _addInputControl(folder, inputElem, name, type, valStr)
    {
        const props = {};
        try
        {
            switch (type)
            {
                case 'float':
                {
                    props[name] = parseFloat(valStr) || 0;
                    const min = parseFloat(inputElem.getAttribute('uisoftmin') ?? inputElem.getAttribute('uimin') ?? '0') || 0;
                    const max = parseFloat(inputElem.getAttribute('uisoftmax') ?? inputElem.getAttribute('uimax') ?? '1') || 1;
                    const step = (max - min) / 1000;
                    folder.add(props, name, min, max, step).name(name).onChange(val =>
                    {
                        inputElem.setAttribute('value', String(val));
                    });
                    break;
                }
                case 'integer':
                {
                    props[name] = parseInt(valStr) || 0;
                    folder.add(props, name).name(name).step(1).onChange(val =>
                    {
                        inputElem.setAttribute('value', String(Math.round(val)));
                    });
                    break;
                }
                case 'boolean':
                {
                    props[name] = valStr === 'true';
                    folder.add(props, name).name(name).onChange(val =>
                    {
                        inputElem.setAttribute('value', val ? 'true' : 'false');
                    });
                    break;
                }
                case 'color3':
                {
                    const parts = valStr.split(/\s+/).map(parseFloat);
                    const r = Math.round((parts[0] || 0) * 255);
                    const g = Math.round((parts[1] || 0) * 255);
                    const b = Math.round((parts[2] || 0) * 255);
                    props[name] = '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
                    folder.addColor(props, name).name(name).onChange(val =>
                    {
                        const hex = val.replace('#', '');
                        const fr = parseInt(hex.slice(0, 2), 16) / 255;
                        const fg = parseInt(hex.slice(2, 4), 16) / 255;
                        const fb = parseInt(hex.slice(4, 6), 16) / 255;
                        inputElem.setAttribute('value', `${fr} ${fg} ${fb}`);
                    });
                    break;
                }
                case 'color4':
                {
                    const parts = valStr.split(/\s+/).map(parseFloat);
                    const r = Math.round((parts[0] || 0) * 255);
                    const g = Math.round((parts[1] || 0) * 255);
                    const b = Math.round((parts[2] || 0) * 255);
                    const a = parts[3] !== undefined ? parts[3] : 1;
                    props[name] = '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
                    folder.addColor(props, name).name(name).onChange(val =>
                    {
                        const hex = val.replace('#', '');
                        const fr = parseInt(hex.slice(0, 2), 16) / 255;
                        const fg = parseInt(hex.slice(2, 4), 16) / 255;
                        const fb = parseInt(hex.slice(4, 6), 16) / 255;
                        inputElem.setAttribute('value', `${fr} ${fg} ${fb} ${a}`);
                    });
                    break;
                }
                case 'vector2':
                case 'vector3':
                case 'vector4':
                {
                    const components = valStr.split(/\s+/).map(parseFloat);
                    const labels = ['x', 'y', 'z', 'w'];
                    const count = type === 'vector2' ? 2 : type === 'vector3' ? 3 : 4;
                    for (let i = 0; i < count; i++)
                    {
                        const key = `${name}_${labels[i]}`;
                        props[key] = components[i] || 0;
                        folder.add(props, key).name(`${name}.${labels[i]}`).onChange(() =>
                        {
                            const updated = Array.from({ length: count }, (_, j) => props[`${name}_${labels[j]}`]);
                            inputElem.setAttribute('value', updated.join(' '));
                        });
                    }
                    break;
                }
                default:
                {
                    props[name] = valStr || '';
                    folder.add(props, name).name(name).onChange(val =>
                    {
                        inputElem.setAttribute('value', val);
                    });
                    break;
                }
            }
        }
        catch (e)
        {
            console.warn('MtlxGraphView: could not add GUI control for', name, e);
        }
    }

    _clearGraph(message)
    {
        this._graph.clear();
        const infoNode = LiteGraph.createNode('basic/string');
        infoNode.title = 'MaterialX Graph';
        infoNode.properties.value = message;
        infoNode.pos = [40, 40];
        infoNode.size = [320, 60];
        this._graph.add(infoNode);
        this._graph.start();
        this._canvas.setDirty(true, true);
    }
}
