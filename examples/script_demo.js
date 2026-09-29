// BenchBoard 示例压测脚本
//
// 用法：在界面里填「目标 URL」和「并发 VU」，点「开始压测」。
//
// ★ 三个参数的优先级（从高到低）：
//     1. 命令行传入的 --vus / --duration      ← 界面的两个输入框走这条
//     2. 命令行传入的 -e BASE_URL=...          ← 界面的 URL 输入框走这条
//     3. 脚本里 options / 下面那行 const       ← 只是兜底默认值
//
// 所以：界面上的三个输入框都是真正生效的，脚本里的值只在没有命令行参数时才用。
//
// 关于 __ENV：k6 用 -e KEY=VALUE 传环境变量进来，脚本里通过 __ENV.KEY 读。
//   取值时一定要带兜底（|| '默认值'），否则直接命令行跑 k6 时会得到 undefined，
//   请求会打到 undefined 上 —— 报错信息还很难看懂。
//
// 被压的本地服务可以用：
//   python -m http.server 8899

import http from 'k6/http';
import { check } from 'k6';

// 目标 URL：界面通过 -e BASE_URL=<输入框内容> 传进来
const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:8899/';

export const options = {
  // 这两个值会被命令行的 --vus / --duration 覆盖，保留只是为了能单独跑脚本
  vus: 10,
  duration: '30s',

  // thresholds 是可选的：不满足时 k6 会以非 0 退出码结束。
  // 本项目不把它当失败处理，但指标会出现在 summary 的 thresholds 字段里，
  // 是后续做「压测通过 / 不通过」判定的天然抓手。
  thresholds: {
    http_req_failed: ['rate<0.01'],
    http_req_duration: ['p(95)<500'],
  },
};

export default function () {
  const res = http.get(BASE_URL);

  // check 的名字会出现在 summary.json 的 root_group.checks 里，
  // 报告中的「断言结果」表就是从那里来的 —— 所以名字要写成可读的句子。
  check(res, {
    'status is 200': (r) => r.status === 200,
  });
}
