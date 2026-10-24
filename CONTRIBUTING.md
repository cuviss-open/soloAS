# Contributing / 参与贡献

- Open an [issue](https://github.com/cuviss-open/soloAS/issues) with the soloAS version, engine version/source, OS, reproduction, expected result and actual result. / 请提交 [问题](https://github.com/cuviss-open/soloAS/issues)，注明 soloAS 版本、引擎版本及来源、操作系统、复现步骤、预期结果和实际结果。
- Submit focused pull requests against `main`; maintainers use the same PR process for exported changes. / 请向 `main` 提交范围集中的 PR；维护者导出的改动也遵循同一 PR 流程。
- Before merging an external PR, maintainers import it into the private development workspace and run the relevant test suite. Results are reported on the public PR before it is merged on GitHub. / 合并外部 PR 前，维护者会将其导入私有开发环境并运行相关测试。测试结果会先在公开 PR 中公布，再于 GitHub 合并。
- Keep the original contributor as author; do not replace their authorship during import or merge. / 导入或合并时保留原贡献者的作者身份，不替换署名。
- PRs must pass **Documentation links** and **License headers**. Preserve existing notices and licenses; add an appropriate SPDX license header to new source files. See [NOTICE](NOTICE) and [LICENSE](LICENSE). / PR 必须通过 **文档链接** 和 **许可头** 检查。保留现有声明和许可证；为新源码文件添加适当的 SPDX 许可头。参见 [NOTICE](NOTICE) 和 [LICENSE](LICENSE)。
- Release tags are created only on commits merged into `main`. / 仅在已合入 `main` 的提交上创建发行标签。
- Remove secrets, private project content and personal information from logs and samples. / 请从日志和示例中移除密钥、私有项目内容及个人信息。
- Contact: [contact@cuviss.com](mailto:contact@cuviss.com). / 联系邮箱：[contact@cuviss.com](mailto:contact@cuviss.com)。

- Contributions use the license of their component: Runtime MIT (embedded AngelScript library zlib), GAS Apache-2.0 OR MIT, EnhancedInput MIT, and SoloASBridge MPL-2.0. / 贡献采用所属组件的许可证：Runtime 为 MIT（内嵌 AngelScript 库为 zlib），GAS 为 Apache-2.0 或 MIT，EnhancedInput 为 MIT，SoloASBridge 为 MPL-2.0。
